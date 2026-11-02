/*
 * The hardware video encoder of the POCO X3 Pro (vayu, SM8150-AC): Venus / IRIS1.
 * bsd/sys/vayu_venus.h is the interface and the boot-args.  Nothing here is touched unless
 * boot-arg vayu_venus asks for it.  Level 1 does M1 only: power, firmware through PAS, the
 * HFI queues behind our own apps-SMMU context, SYS_INIT and the version string, then
 * everything down again; it opens no session and moves no frame.  Level 2 adds one encode
 * session and one generated NV12 frame (M2/M3): SESSION_INIT, the encoder's properties, the
 * firmware's BUFFER_REQUIREMENTS, SET_BUFFERS, LOAD_RESOURCES, START, EMPTY_BUFFER and
 * FILL_BUFFER, and logs the bitstream's NAL units, its first 64 bytes and, if it can, writes
 * it to /private/var/tmp/venus_out.h265.
 *
 * The hardware, with its sources (FACT unless marked HYPOTHESIS):
 *
 *   block       0x0aa00000, 0x200000 bytes (vendor sm8150-vidc.dtsi:18-21, reg and
 *               interrupts = GIC_SPI 174 level-high).  The wrapper is at +0xe0000
 *               (mainline hfi_venus_io.h:77-78), the CPU's control/status page at
 *               +0xc0000+0x12000 and its interrupt controller at +0xc0000+0x1f000
 *               (hfi_venus_io.h:18-21).  HFI 4xx, not 6xx: the vendor block is the v4
 *               register map (wrapper 0xe0000, CPU 0xc0000), the vendor driver is HFI 4xx,
 *               and the mvs0/mvs1/mvsc GDSCs are the v4 split (sm8150-vidc.dtsi:29-31).
 *               Mainline has no sm8150 resource entry; its HFI 4XX entries are sdm845-v2
 *               and sc7180 (core.c:696-765), while sm8250 is HFI 6XX (core.c:799-824) --
 *               the "sm8250" of the task is a 6xx/IRIS2 part, not this one.  That SM8150
 *               is IRIS1 is a HYPOTHESIS; the v4 programming used here does not depend on
 *               the name, only on the non-V6 offsets.
 *
 *   clocks      GCC video AHB CBCR 0xb004 bit 0, critical (gcc-sm8150.c:3841-3855); GCC
 *               video XO 0xb040 (gcc-sm8150.c:3896-3906); GCC video axic/axi0/axi1
 *               0xb02c/0xb024/0xb028 (gcc-sm8150.c:3857-3894).  video_cc's iris RCG at
 *               0x7f0 (videocc-sm8150.c:174-197), its AHB branch 0x8f4, mvsc core 0x850,
 *               mvs0 core 0x890, mvs1 core 0x8d0 and XO 0x984 (videocc-sm8150.c:199-283).
 *               CBCR bit 0 enable, bit 31 clock-off (clk-branch.c:55, :60-77).  PLL0 is a
 *               Trion PLL at 0x42c, L 0x14 / alpha 0xD555 for 400 MHz (2 x the 200 MHz iris
 *               rate), the DT's lowest rate (videocc-sm8150.c:106-118, :131-152, :154-161).
 *
 *   GDSCs       mvsc 0xab00814, mvs0 0xab00874, mvs1 0xab008b4 (sm8150-gdsc.dtsi:239-259,
 *               live.dts:726-775), all parent-supply VDD_MMCX_LEVEL (sm8150.dtsi:4116-4156).
 *               GDSCR bit 31 PWR_ON, bit 1 HW_CONTROL (the hardware trigger), bit 2
 *               SW_OVERRIDE, bit 0 SW_COLLAPSE (gdsc.c:26-32).  mvs0 and mvs1 carry
 *               "qcom,support-hw-trigger" on this phone (live.dts:742-775), so the vendor
 *               driver clears HW_CONTROL before touching SW_COLLAPSE and sets it again after
 *               the domain is up (gdsc.c:59-64, :197-210, :222-241) -- the firmware then
 *               collapses the core itself.  mvs0/mvs1 also need the wrapper's MMCC
 *               handshake: write 0 to control and poll status bit 1 (hfi_venus_io.h:139-144,
 *               pm_helpers.c:409-440).
 *
 *   rails       mmcx.lvl is PM8150L S5, "the VDD_MM_CX supply" (sm8150-regulator.dtsi:578-
 *               601, live.dts:18385-18399), parent mx.lvl (PM8150L S4, :526-567,
 *               live.dts:18340-18353); both are shared and are never voted down from here.
 *
 *   buses       the video path crosses four RPMh BCMs, and no clock in this file makes it
 *               usable: VIDEO_P0/VIDEO_PROC are on MM3 (sm8150-bus.dtsi:1199-1242), the LLCC
 *               slave on SH0 (:1977-1985), DDR (slv-ebi) on MC0 and ACV (:2004-2012).  The
 *               vendor resolves those names in the RPMh command DB (msm_bus_fabric_rpmh.c:
 *               1195-1205); the command is commit/valid plus two 14-bit votes, and this
 *               port's hakios.cpufreq.c already uses those four resources on this phone
 *               with the values copied here (its :433-437, :515-521).  MM3 is raised and
 *               given back; SH0/MC0/ACV are the whole system's memory path and are only
 *               ever raised here, like the rails.  (HYPOTHESIS: the bootloader does not
 *               leave the video NoC voted for a kernel that boots from UEFI, and without it
 *               the ARM9 cannot fetch its image.)
 *
 *   SMMU        the apps SMMU at 0x15000000 (vayu_camera.c's smmu_find()/smmu_up(), the same
 *               block and the same IDR0/IDR1-driven layout this file reuses).  The non-secure
 *               Venus context is **stream 0x2300, mask 0x60** on this phone: the live device
 *               tree says iommus = <&apps_smmu 0x2300 0x60> (live.dts:25266-25271), while
 *               the vendor sm8150-vidc.dtsi:95-101 still prints 0x1300 -- the DT that UEFI
 *               hands the kernel is the 0x2300 one and is what the firmware's SID decodes to.
 *               Virtual-addr-pool 0x25800000 + 0xba800000 (both).  The queue table and the
 *               three queues are one 38 MB contiguous block behind IOVA 0x25800000, normal
 *               non-cacheable, outer shareable, exactly the camera step-3 PTE
 *               (vayu_camera.c's CAM_PTE).  On our side the same memory is mapped
 *               VM_WIMG_WCOMB, as the PAS loader maps the firmware (vayu_pas.c).
 *
 *   firmware    venus.mdt + venus.b00-b04, ELF32 ARM, entry 0xf500000; the loadable
 *               segments are paddr 0xf500000 (0x103118), 0xf604000 (file 0x9e44, mem
 *               0x3f4000) and 0xf9ff000 (0x20), the hash segment at 0xfa00000 (0x1a98,
 *               mem 0x2000); all loadable segments carry BIT(27) (QCOM_MDT_RELOCATABLE), so
 *               the image is placed at the start of its memory and its extent is exactly
 *               0x500000, the size of pil_video_region@97600000 (sm8150.dtsi:639-643).
 *               The image is split (the .mdt is 7020 bytes; b02/b03/b04 are the three
 *               loadable files); venus.b00 (0xd4) is the header's first phdr and venus.b01
 *               (0x1a98) the hash segment -- vayu_pas.c does not load either separately.
 *               SHA-256 and the exact paths are in notes/VENUS_DESIGN.md section 2.
 *
 *   PAS         venus@aae0000 is qcom,pil-tz-generic, pas-id 9, firmware-name "venus",
 *               memory-region pil_video_mem, clocks xo/core/ahb at 200 MHz
 *               (sm8150.dtsi:1911-1938); mainline VENUS_PAS_ID is 9 (firmware.c:24).  Its
 *               image is loaded by vayu_pas_load() (INIT_IMAGE, MEM_SETUP, write+readback);
 *               AUTH_AND_RESET is the one step vayu_pas.c does not export for a processor
 *               without an SMP2P item, so it is done here with the same SMC encoding
 *               (vayu_pas.c:1127-1139).  After it, mainline's venus_boot() calls
 *               qcom_scm_set_remote_state(1, 0) (firmware.c:210-264, qcom_scm.c:394-411);
 *               that call is made here too and ignored if TrustZone refuses it (HYPOTHESIS
 *               that it is optional for Venus; mainline ignores -EINVAL).
 *
 *   HFI         three queues (cmd 0, msg 1, dbg 2), 1024-byte packets, 50 buffers, 16
 *               clients: a 24-byte table header, three 56-byte queue headers, then three
 *               0xc8000-byte rings; table header {version 0, size 0xc0, qhdr0 0x18,
 *               qhdr_size 0x38, num_q 3, num_active_q 3}, each queue header {status 1,
 *               start_addr, type 0x01010000|i, q_size 0x32000 dwords, pkt_size 0, ...,
 *               rx_req 1, read/write 0} (mainline hfi_venus.c:32-94, :747-783, :817-821).
 *               UC_REGION_ADDR 0x64 = table IOVA, UC_REGION_SIZE 0x68 = 0x2600000,
 *               CPU_CS_SCIACMDARG2 0x54 = table IOVA, ARG1 0x50 = 1, SFR_ADDR 0x5c = SFR
 *               IOVA, VIDC_CTRL_INIT 0x48 = 1, poll CPU_CS_SCIACMDARG0 0x4c
 *               (hfi_venus_io.h:28-57, hfi_venus.c:521-556).  The host-to-firmware
 *               doorbell is CPU_IC_SOFTINT 0x18 bit 15 (hfi_venus_io.h:69-73,
 *               hfi_venus.c:368-378).
 *
 *   packets     SYS_INIT {12, 0x10001, HFI_VIDEO_ARCH_OX = 1}; SYS_INIT done
 *               {size, 0x20001, error_type, num_properties, data...}; SYS_SET_PROPERTY
 *               debug {24, 0x10005, 1, HFI_PROPERTY_SYS_DEBUG_CONFIG = 1, config, mode}
 *               and codec power plane {20, 0x10005, 1, 5, enable}; SYS_GET_PROPERTY
 *               {16, 0x10006, 1, HFI_PROPERTY_SYS_IMAGE_VERSION = 6}; its answer is
 *               {size, 0x2000a, num_properties, 6, data...} (hfi_cmds.c:14-19, :38-50,
 *               :125-134, :154-160; hfi_cmds.h:46-79; hfi_msgs.h:10-18, :62-67, :113-118;
 *               hfi_helper.h:15, :423-428, :624-629).  SYS_PING {12, 0x210002, 0xbeef}
 *               and its ack {12, 0x220002, 0xbeef} (hfi_cmds.c:145-152, hfi_msgs.c:330-350)
 *               are a liveness check.
 *
 * The firmware-to-host line is left MASKED in the wrapper (WRAPPER_INTR_MASK 0x10 = 0)
 * and the message queue is polled: there is no GIC handler for SPI 174 yet, and a level
 * line left asserted on an unhandled SPI is a boot hazard.  M2 installs the handler and
 * unmasks A2HVCODEC (0x8) (hfi_venus.c:456-500).
 *
 * The packets and the queue table are built and parsed by sys/vayu_venus_hfi.h, which
 * tools/venus-hfi compiles and checks word for word on the build machine: the bytes here
 * are the bytes tested there.
 *
 * The safety rules: every write that matters is read back; every poll is bounded; a GDSC,
 * branch, PLL or match register that somebody else has is left alone; a shared rail is
 * never voted down; on any failure the teardown runs in reverse and logs the step; the
 * firmware is stopped before its memory, SMMU or clocks are touched.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/vayu_pas.h>
#include <sys/vayu_scm.h>
#include <sys/vayu_venus.h>
#include <sys/vayu_venus_hfi.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern void loader_log_putc(char c);
extern void loader_log_line(const char *line);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern lck_mtx_t hakios_smmu_lock;
extern kern_return_t thread_terminate(thread_t thread);
extern void IOSleep(unsigned int ms);
extern void delay(int us);

/* Every line also goes into the loader's log (as vayu_slpi.c's say() does). */
static bool g_leaving;                  /* on the loader-return path: the loader's log only, no printf */
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
	if (!g_leaving) {
		printf("%s", line);
	}
	return length;
}

/* ---- the blocks --------------------------------------------------------------------------------- */

#define VENUS_PHYS              0x0aa00000UL    /* dt: qcom,vidc@aa00000 (sm8150-vidc.dtsi:18-21) */
#define VENUS_SIZE              0x100000UL      /* the DT says 0x200000, which would overlap video_cc at
                                                 * 0xab00000 (sm8150.dtsi:1436); the last register used
                                                 * is 0xdf01c, so 1 MB is the whole block we touch */
#define GCC_PHYS                0x00100000UL    /* vayu_camera.c:368, vayu_bt.c:66 */
#define GCC_SIZE                0x80000UL
#define VIDEOC_PHYS             0x0ab00000UL    /* sm8150.dtsi:1434-1436 */
#define VIDEOC_SIZE             0x10000UL
#define APPS_SMMU_PHYS          0x15000000UL    /* vayu_camera.c:2317 */
#define APPS_SMMU_SIZE          0x2000UL

#define WRAPPER_BASE            0x000e0000      /* hfi_venus_io.h:77-78, the v4 layout */
#define WRAPPER_HW_VERSION      0x00
#define WRAPPER_INTR_STATUS     0x0c
#define WRAPPER_INTR_MASK       0x10
#define WRAPPER_INTR_CLEAR      0x14
#define WRAPPER_VCODEC0_STATUS  0x90            /* hfi_venus_io.h:139-144 */
#define WRAPPER_VCODEC0_CTRL    0x94
#define WRAPPER_VCODEC1_STATUS  0x110
#define WRAPPER_VCODEC1_CTRL    0x114

#define CPU_BASE                0x000c0000      /* hfi_venus_io.h:18-21 */
#define CPU_CS_BASE             (CPU_BASE + 0x12000)
#define CPU_IC_BASE             (CPU_BASE + 0x1f000)
#define CPU_CS_A2HSOFTINTCLR    0x1c
#define CPU_IC_SOFTINT          0x18
#define CPU_IC_SOFTINT_H2A      (1u << 15)
#define VIDC_CTRL_INIT          0x48
#define CPU_CS_SCIACMDARG0      0x4c
#define CPU_CS_SCIACMDARG1      0x50
#define CPU_CS_SCIACMDARG2      0x54
#define SFR_ADDR                0x5c
#define UC_REGION_ADDR          0x64
#define UC_REGION_SIZE          0x68

#define VENUS_INTR_ALL          0x1c            /* A2HWD | A2HVCODEC | A2HCPU; mask all for M1 */

/* ---- small register helpers (as vayu_camera.c's) ------------------------------------------------- */

static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
	__asm__ volatile ("dmb sy" : : : "memory");
}

/* Wait for (value & mask) == mask (set) or == 0 (!set); bounded at us microseconds. */
static bool
wait_bits(vm_offset_t base, uint32_t offset, uint32_t mask, bool set, unsigned us, uint32_t *last)
{
	for (unsigned i = 0; i < us / 10 + 1; i++) {
		uint32_t value = rd(base + offset);

		if (set ? (value & mask) == mask : (value & mask) == 0) {
			*last = value;
			return true;
		}
		delay(10);
	}
	*last = rd(base + offset);
	return false;
}

/* A register written, then read back under `check'; false and a line if it did not take. */
static bool
put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("vayu-venus: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		return false;
	}
	return true;
}

/* ---- what the boot args asked for ----------------------------------------------------------------- */

static uint32_t g_level;                /* vayu_venus: 0 off, 1 M1, 2 M1+M2+M3 (one frame) */
static uint32_t g_wait_ms = 3000;       /* vayu_venus_wait */
static uint32_t g_secs = 2;             /* vayu_venus_secs */
static uint32_t g_rails = 1;            /* vayu_venus_rails */
static uint32_t g_rail_level = 384;     /* vayu_venus_level: RPMH_REGULATOR_LEVEL_TURBO */
static uint32_t g_rate_mhz = 200;       /* vayu_venus_rate */
static uint32_t g_xo;                   /* vayu_venus_xo=1: no PLL0 */
static uint32_t g_gdsc_mask = 1;        /* vayu_venus_gdsc: bit 0 mvsc, 1 mvs0, 2 mvs1 */
static uint32_t g_debug = 0x18;         /* vayu_venus_debug: ERROR | FATAL */
static uint32_t g_bcm = 1;              /* vayu_venus_bcm=0: no video NoC/DDR BCM vote */

/* The one frame of M3: 1080p30 NV12 to HEVC unless the boot args say otherwise. */
static uint32_t g_codec = VAYU_HFI_CODEC_HEVC;  /* vayu_venus_codec: 0 H.264, 1 HEVC */
static uint32_t g_width = 1920;         /* vayu_venus_width */
static uint32_t g_height = 1080;        /* vayu_venus_height */
static uint32_t g_fps = 30;             /* vayu_venus_fps */
static uint32_t g_bitrate = 8000000;    /* vayu_venus_bitrate, bits per second */
static uint32_t g_frame_ms = 3000;      /* vayu_venus_frame_ms: the wait for FILL_BUFFER done */
static uint32_t g_dump = 1;             /* vayu_venus_dump=0: do not write /private/var/tmp/venus_out.h265 */

/* ---- state, so the teardown knows exactly what to put back ---------------------------------------- */

static vm_offset_t gcc_va, videoc_va, venus_va;

static const char *const g_rails_names[2] = { "mmcx.lvl", "mx.lvl" };
static bool g_rails_voted;

static bool g_gcc_ahb, g_gcc_xo, g_gcc_axic, g_gcc_axi0, g_gcc_axi1;
static bool g_gdsc_ours[3], g_wrapper_ours[3];
static bool g_gdsc_hw_old[3];           /* HW_CONTROL (bit 1) as it was found */
static uint32_t g_wrapper_old[3];
static bool g_pll0_ours, g_rcg_set;
static uint32_t g_rcg_old;
static bool g_br_iris_ahb, g_br_mvsc_core, g_br_mvs0_core, g_br_mvs1_core;
static bool g_br_video_cc_xo;

/* The BCMs voted (their cmd-db addresses, 0: the name is not in the database). */
static uint32_t g_bcm_address[4];
static bool g_bcm_voted;

static bool g_power_started, g_power_up;
static bool g_smmu_on;
static uint32_t g_smmu_match = ~0u, g_smmu_context_n, g_smmu_old_smr, g_smmu_old_s2cr, g_smmu_old_sctlr;
static vm_offset_t g_smmu_va, g_smmu_second, g_smmu_context, g_smmu_tables;
static uint64_t g_smmu_tables_bytes;

static vm_offset_t g_shared_kva;        /* the kmem_alloc_contig() address (cached, never touched) */
static vm_offset_t g_shared;            /* the same memory, VM_WIMG_WCOMB: every queue access */
static uint64_t g_shared_bytes;
static uint32_t g_table_iova, g_sfr_iova;

/*
 * M2/M3's buffers: one contiguous block of ours, right after the 38 MB HFI block in the same
 * venus_ns IOVA pool (0x27e00000 = 0x25800000 + 0x2600000), mapped by the same SMMU context.
 * The CPU alias is cached: every hand-off does its own cache maintenance (clean before the
 * firmware reads, invalidate before we read what it wrote).
 */
static vm_offset_t g_pool_kva;
static uint64_t g_pool_bytes, g_pool_used;
static uint32_t g_pool_iova;

static bool g_queues_ready, g_firmware_up, g_inited;

/* The PAS description is filled in below; the firmware path needs it before that. */
static struct vayu_rproc venus;

/* ---- RPMh: mmcx.lvl and mx.lvl, shared, never given back ----------------------------------------- */

static bool
rails_up(void)
{
	if (g_rails == 0) {
		say("vayu-venus: 1: RPMh: not voted (vayu_venus_rails=0); the island's rails are taken as found\n");
		return true;
	}
	if (!vayu_rpmh_vote(g_rails_names, 2, g_rail_level)) {
		say("vayu-venus: 1: FAILED: RPMh did not take mmcx.lvl/mx.lvl at level %u\n", g_rail_level);
		return false;
	}
	g_rails_voted = true;
	say("vayu-venus: 1: RPMh: mmcx.lvl and mx.lvl at level %u; shared, never voted down from here\n", g_rail_level);
	return true;
}

/* ---- RPMh: the video NoC and DDR BCMs ------------------------------------------------------------ */

/*
 * The BCM command is "commit, valid, average and peak, 14 bits each" (Linux icc-bcm-voter,
 * quoted in this port's hakios.cpufreq.c:420-424): commit is bit 30, valid bit 29, the two
 * votes bits 27:14 and 13:0.  The values below are the ones that file already puts on this
 * phone's MC0/SH0/ACV on real hardware (hakios.cpufreq.c:433-437, :515-521), and its measured
 * effect is recorded there (12.4 -> 15.4 GB/s writing with the maximum).  "No request" is
 * commit with valid clear (:558).
 */
#define VENUS_BCM_VOTE(x, y)    (0x40000000u | 0x20000000u | \
	                        ((uint32_t)(x) & 0x3fffu) << 14 | ((uint32_t)(y) & 0x3fffu))
#define VENUS_BCM_OFF           0x40000000u

/*
 * The four bus clock managers the video path crosses (vendor sm8150-bus.dtsi):
 *   MM3  mas-qxm-venus0 and mas-qxm-venus-arm9, the two video masters on the MMSS NoC (:1199-1242);
 *   SH0  slv-qns-llcc, the LLCC slave every master with a cache slice goes through (:1977-1985);
 *   MC0  slv-ebi, the DDR controller (:2004-2012);
 *   ACV  the DDR voltage corner voted with MC0 (:2004-2012).
 * Their addresses are not in the DT: the vendor driver looks each name up in the RPMh command
 * database (msm_bus_fabric_rpmh.c:1205 cmd_db_get_addr()).  A name the database does not have is
 * logged and skipped, and M1 goes on: the bootloader may have left the path voted (HYPOTHESIS).
 *
 * MM3 is the video's own and is given back when M1 ends.  SH0/MC0/ACV are the whole system's
 * memory path (hakios.cpufreq.c votes them for the GPU and the cores, and the loader needs them):
 * M1 only ever raises them, exactly as it leaves mmcx.lvl alone, and never votes them down.
 */
static const char *const g_bcm_names[4] = { "MM3", "SH0", "MC0", "ACV" };
static const uint32_t g_bcm_values[4] = {
	VENUS_BCM_VOTE(0x3fff, 0x3fff),         /* MM3: the highest average and peak (msm_bus clamps both to 14 bits) */
	VENUS_BCM_VOTE(0x3fff, 0x3fff),         /* SH0: as hakios.cpufreq.c's ddr vote */
	VENUS_BCM_VOTE(0x3fff, 0x3fff),         /* MC0: the same */
	VENUS_BCM_VOTE(0, 8),                   /* ACV: the apps' corner bit only (BCM_ACV_APPS) */
};

static bool
bcm_up(void)
{
	uint32_t addresses[4], values[4];
	unsigned found = 0;

	if (g_bcm == 0) {
		say("vayu-venus: 1: BCM: not voted (vayu_venus_bcm=0); the video NoC/DDR path is taken as found\n");
		return true;
	}
	for (unsigned i = 0; i < 4; i++) {
		g_bcm_address[i] = vayu_rpmh_address(g_bcm_names[i]);
		if (g_bcm_address[i] != 0) {
			addresses[found] = g_bcm_address[i];
			values[found] = g_bcm_values[i];
			found++;
		} else {
			say("vayu-venus: 1: BCM %s is not in the command database: left as found\n", g_bcm_names[i]);
		}
	}
	if (found == 0) {
		say("vayu-venus: 1: BCM: none of MM3/SH0/MC0/ACV is known here; going on without a vote\n");
		return true;
	}
	if (!vayu_rpmh_send(addresses, values, found, "venus video NoC/DDR")) {
		say("vayu-venus: 1: FAILED: RPMh did not take the video NoC/DDR BCM votes (%u of 4 asked)\n", found);
		return false;
	}
	g_bcm_voted = true;
	say("vayu-venus: 1: BCM: %u of 4 voted (MM3 %#x, SH0/MC0 %#x, ACV %#x); SH0/MC0/ACV are shared and stay\n",
	    found, g_bcm_values[0], g_bcm_values[1], g_bcm_values[3]);
	return true;
}

static void
bcm_down(void)
{
	uint32_t value = VENUS_BCM_OFF;

	if (!g_bcm_voted) {
		return;
	}
	g_bcm_voted = false;
	if (g_bcm_address[0] == 0) {
		return;                         /* MM3 was not there: nothing of this file's to give back */
	}
	if (!vayu_rpmh_send(&g_bcm_address[0], &value, 1, "venus MM3 off")) {
		say("vayu-venus: 1: the video NoC's MM3 vote could not be given back\n");
	} else {
		say("vayu-venus: 1: BCM: MM3 given back; SH0/MC0/ACV left (the whole system's memory path)\n");
	}
}

/* ---- GCC video branches --------------------------------------------------------------------------- */

struct venus_gcc_branch {
	const char *name;
	uint32_t cbcr;
	bool *ours;
	bool delay;             /* BRANCH_HALT_DELAY (AHB, XO): no status poll on enable */
};

static struct venus_gcc_branch g_gcc[] = {
	{ "gcc video_ahb",  0xb004, &g_gcc_ahb,  true  },       /* gcc-sm8150.c:3841-3855 */
	{ "gcc video_xo",   0xb040, &g_gcc_xo,   true  },       /* gcc-sm8150.c:3896-3906 */
	{ "gcc video_axic", 0xb02c, &g_gcc_axic, false },       /* gcc-sm8150.c:3883-3894 */
	{ "gcc video_axi0", 0xb024, &g_gcc_axi0, false },       /* gcc-sm8150.c:3857-3868 */
	{ "gcc video_axi1", 0xb028, &g_gcc_axi1, false },       /* gcc-sm8150.c:3870-3881 */
};

static bool
gcc_branch_up(struct venus_gcc_branch *b)
{
	uint32_t value = rd(gcc_va + b->cbcr);

	if ((value & 1u) != 0) {
		say("vayu-venus: 1: %s: cbcr %#x = %#x (on already, left on)\n", b->name, b->cbcr, value);
		return true;
	}
	*b->ours = true;
	wr(gcc_va + b->cbcr, value | 1u);
	if (b->delay) {
		delay(10);
		value = rd(gcc_va + b->cbcr);
		/* A hardware-gated branch may leave the enable bit 0 while CLK_OFF is 0. */
		if ((value & 1u) == 0 && (value & (1u << 31)) != 0) {
			say("vayu-venus: 1: FAILED: %s does not take its enable (cbcr %#x = %#x)\n",
			    b->name, b->cbcr, value);
			return false;
		}
	} else if (!wait_bits(gcc_va, b->cbcr, 1u << 31, false, 500, &value)) {
		say("vayu-venus: 1: FAILED: %s does not come on (cbcr %#x = %#x)\n", b->name, b->cbcr, value);
		return false;
	}
	say("vayu-venus: 1: %s: cbcr %#x -> %#x (on)\n", b->name, b->cbcr, value);
	return true;
}

/* The AHB first (video_cc's registers sit behind it), then, after the GDSCs, the rest. */
static bool
gcc_video_ahb_up(void)
{
	return gcc_branch_up(&g_gcc[0]);
}

static bool
gcc_video_rest_up(void)
{
	for (unsigned i = 1; i < sizeof(g_gcc) / sizeof(g_gcc[0]); i++) {
		if (!gcc_branch_up(&g_gcc[i])) {
			return false;
		}
	}
	return true;
}

static void
gcc_video_down(void)
{
	for (int i = (int)(sizeof(g_gcc) / sizeof(g_gcc[0])) - 1; i >= 0; i--) {
		struct venus_gcc_branch *b = &g_gcc[i];
		uint32_t value;

		if (!*b->ours) {
			continue;
		}
		value = rd(gcc_va + b->cbcr);
		wr(gcc_va + b->cbcr, value & ~1u);
		if (!b->delay) {
			(void)wait_bits(gcc_va, b->cbcr, 1u << 31, true, 500, &value);
		} else {
			delay(10);
			value = rd(gcc_va + b->cbcr);
		}
		say("vayu-venus: 1: %s off (cbcr %#x = %#x)\n", b->name, b->cbcr, value);
		*b->ours = false;
	}
}

/*
 * A block reset (BCR): reset.c's assert sets the bit, deassert clears it, and the vendor
 * reset map puts the video core's at video_cc 0x850 bit 2 and the three AXI bridges' at
 * GCC 0xb02c/0xb024/0xb028 bit 2 (videocc-sm8150.c:303-305, gcc-sm8150.c:4164-4166).
 * Pulse each one after the clocks are on and before the ARM9 is released.
 */
static void
venus_reset_pulse(vm_offset_t base, const char *name, uint32_t offset)
{
	uint32_t before = rd(base + offset), after;

	wr(base + offset, before | (1u << 2));
	delay(200);
	wr(base + offset, rd(base + offset) & ~(1u << 2));
	delay(200);
	after = rd(base + offset);
	say("vayu-venus: 1: reset %s (%#x): %#x -> %#x, bit 2 %s\n", name, offset, before, after,
	    (after & (1u << 2)) == 0 ? "released" : "STILL ASSERTED");
}

/* ---- video_cc's GDSCs ----------------------------------------------------------------------------- */

/*
 * GDSCR bits (gdsc.c:26-32): 31 PWR_ON, 2 SW_OVERRIDE, 1 HW_CONTROL, 0 SW_COLLAPSE.
 * mvs0 and mvs1 carry "qcom,support-hw-trigger" on vayu (live.dts:742-762), so while
 * HW_CONTROL is set SW_COLLAPSE is not the master: the vendor driver clears HW_CONTROL,
 * toggles the switch, polls PWR_ON and then sets HW_CONTROL again (gdsc.c:59-64,
 * :197-210, :222-241 -> gdsc-regulator.c:911-913).  The old code wrote SW_COLLAPSE with
 * bit 1 still set, which the hardware ignores: that is the first thing this port fixes.
 */
struct venus_gdsc {
	const char *name;
	uint32_t offset;        /* in video_cc */
	uint32_t mask;          /* the vayu_venus_gdsc bit that asks for it */
	uint32_t ctrl;          /* wrapper MMCC control, 0: none */
	uint32_t status;        /* wrapper MMCC status */
	bool hw_trigger;        /* dt: qcom,support-hw-trigger */
};

static struct venus_gdsc g_gdsc[] = {
	{ "mvsc (iris-ctl)", 0x814, 1u << 0, 0,          0,    false },  /* sm8150-gdsc.dtsi:239-245, live.dts:726-740 */
	{ "mvs0 (vcodec)",   0x874, 1u << 1, 0x94,       0x90, true  },  /* sm8150-gdsc.dtsi:246-252, live.dts:742-757 */
	{ "mvs1 (cvp)",      0x8b4, 1u << 2, 0x114,      0x110, true },  /* sm8150-gdsc.dtsi:253-259, live.dts:759-775 */
};

/* Record whether the hardware trigger was on, and take it off for SW_COLLAPSE to be obeyed. */
static void
gdsc_hw_record(struct venus_gdsc *g, unsigned index)
{
	uint32_t value = rd(videoc_va + g->offset);

	g_gdsc_hw_old[index] = (value & (1u << 1)) != 0;
	if (g_gdsc_hw_old[index]) {
		wr(videoc_va + g->offset, value & ~(1u << 1));
		delay(1);
	}
}

/* Take HW_CONTROL off without touching what was found (the way down). */
static void
gdsc_hw_clear(struct venus_gdsc *g)
{
	uint32_t value = rd(videoc_va + g->offset);

	if ((value & (1u << 1)) != 0) {
		wr(videoc_va + g->offset, value & ~(1u << 1));
		delay(1);
	}
}

static void
gdsc_hw_restore(struct venus_gdsc *g, unsigned index)
{
	if (g_gdsc_hw_old[index]) {
		wr(videoc_va + g->offset, rd(videoc_va + g->offset) | (1u << 1));
	}
}

static bool
gdsc_up(struct venus_gdsc *g)
{
	uint32_t value;
	unsigned index = (unsigned)(g - g_gdsc);

	if ((g_gdsc_mask & g->mask) == 0) {
		say("vayu-venus: 1: %s GDSC: not asked for (vayu_venus_gdsc %#x), left as found (%#x)\n",
		    g->name, g_gdsc_mask, rd(videoc_va + g->offset));
		return true;
	}
	value = rd(videoc_va + g->offset);
	if ((value & (1u << 31)) != 0) {
		say("vayu-venus: 1: %s GDSC: on already (%#x), left on\n", g->name, value);
	} else {
		g_gdsc_ours[index] = true;
		if (g->hw_trigger) {
			gdsc_hw_record(g, index);
		}
		/* SW_COLLAPSE cleared, and SW_OVERRIDE (bit 2) too: the hardware state machine sequences. */
		value = rd(videoc_va + g->offset);
		wr(videoc_va + g->offset, value & ~(1u | 1u << 2));
		if (!wait_bits(videoc_va, g->offset, 1u << 31, true, 5000, &value)) {
			say("vayu-venus: 1: FAILED: %s GDSC did not come on (%#x)\n", g->name, value);
			return false;
		}
		say("vayu-venus: 1: %s GDSC on (%#x)\n", g->name, value);
		if (g->hw_trigger) {
			/* now the firmware may collapse it itself (gdsc.c:197-210) */
			wr(videoc_va + g->offset, rd(videoc_va + g->offset) | (1u << 1));
			delay(1);
			say("vayu-venus: 1: %s GDSC hardware trigger on (gdscr %#x)\n", g->name,
			    rd(videoc_va + g->offset));
		}
	}
	if (g->ctrl != 0) {
		g_wrapper_old[index] = rd(venus_va + WRAPPER_BASE + g->ctrl);
		g_wrapper_ours[index] = true;
		wr(venus_va + WRAPPER_BASE + g->ctrl, 0);
		if (!wait_bits(venus_va + WRAPPER_BASE, g->status, 1u << 1, true, 1000, &value)) {
			say("vayu-venus: 1: FAILED: %s MMCC handshake did not acknowledge (status %#x)\n", g->name, value);
			return false;
		}
		say("vayu-venus: 1: %s MMCC handshake: control %#x -> 0, status %#x\n",
		    g->name, g_wrapper_old[index], value);
	}
	return true;
}

static void
gdsc_down(struct venus_gdsc *g)
{
	uint32_t value;
	unsigned index = (unsigned)(g - g_gdsc);

	if (g_wrapper_ours[index] && g->ctrl != 0) {
		wr(venus_va + WRAPPER_BASE + g->ctrl, g_wrapper_old[index]);
		(void)wait_bits(venus_va + WRAPPER_BASE, g->status, 1u << 1,
		    g_wrapper_old[index] == 0, 1000, &value);
		g_wrapper_ours[index] = false;
		say("vayu-venus: 1: %s MMCC handshake put back to %#x (status %#x)\n",
		    g->name, g_wrapper_old[index], value);
	}
	if (!g_gdsc_ours[index]) {
		return;
	}
	if (g->hw_trigger) {
		gdsc_hw_clear(g);               /* hardware trigger off before the collapse */
		(void)wait_bits(videoc_va, g->offset, 1u << 31, true, 1000, &value);
	}
	wr(videoc_va + g->offset, rd(videoc_va + g->offset) | 1u);
	if (!wait_bits(videoc_va, g->offset, 1u << 31, false, 5000, &value)) {
		say("vayu-venus: 1: %s GDSC DID NOT COLLAPSE (%#x)\n", g->name, value);
	} else {
		say("vayu-venus: 1: %s GDSC off (%#x)\n", g->name, value);
	}
	if (g->hw_trigger) {
		gdsc_hw_restore(g, index);      /* if it was hardware-triggered before us, it is again */
	}
	g_gdsc_ours[index] = false;
}

/* ---- video_cc's PLL0 and iris_clk_src -------------------------------------------------------------- */

#define VIDEOC_PLL0             0x42c           /* videocc-sm8150.c:131-136 */
#define PLL_MODE                0x00
#define PLL_L_VAL               0x04
#define PLL_CAL_L_VAL           0x08            /* TRION_PLL_CAL_L_VAL, clk-alpha-pll.c:79 */
#define PLL_USER_CTL            0x0c
#define PLL_USER_CTL_U          0x10
#define PLL_USER_CTL_U1         0x14
#define PLL_CONFIG_CTL          0x18
#define PLL_CONFIG_CTL_U        0x1c
#define PLL_CONFIG_CTL_U1       0x20
#define PLL_TEST_CTL            0x24
#define PLL_TEST_CTL_U          0x28
#define PLL_TEST_CTL_U1         0x2c
#define PLL_OPMODE              0x38
#define PLL_ALPHA_VAL           0x40
#define PLL_OUTCTRL             (1u << 0)
#define PLL_RESET_N             (1u << 2)
#define PLL_HW_UPDATE_BYPASS    (1u << 23)
#define PLL_LOCKED              (1u << 31)
#define PLL_STANDBY             0u
#define PLL_RUN                 1u
#define PLL_OUT_MASK            0x7u

#define VIDEOC_IRIS_CMD         0x7f0           /* videocc-sm8150.c:174-197 */
#define VIDEOC_IRIS_CFG         (VIDEOC_IRIS_CMD + 4)
#define VIDEOC_CFG_MASK         0x371fu         /* [4:0] div, [10:8] src, [13:12] mode */
#define VIDEOC_CMD_UPDATE       (1u << 0)

/* 2 x rate = 19.2 MHz * (L + alpha/65536); the RCG then divides by two (field 3). */
static bool
pll0_rates(uint32_t mhz, uint32_t *l, uint32_t *alpha)
{
	uint64_t vco_khz = (uint64_t)mhz * 2000u;
	uint32_t l_val = (uint32_t)(vco_khz / 19200u);
	uint32_t frac = (uint32_t)(((vco_khz % 19200u) << 16) / 19200u);

	if (mhz < 125 || mhz > 1000 || l_val == 0) {
		return false;
	}
	*l = l_val;
	*alpha = frac;
	return true;
}

static bool
pll0_up(void)
{
	uint32_t l, alpha, mode, opmode, value;
	bool ok = true;

	if (g_xo) {
		say("vayu-venus: 1: PLL0: not started (vayu_venus_xo=1); the iris RCG comes from XO\n");
		return true;
	}
	if (!pll0_rates(g_rate_mhz, &l, &alpha)) {
		say("vayu-venus: 1: FAILED: %u MHz is not a rate this PLL table takes\n", g_rate_mhz);
		return false;
	}
	mode = rd(videoc_va + VIDEOC_PLL0 + PLL_MODE);
	opmode = rd(videoc_va + VIDEOC_PLL0 + PLL_OPMODE);
	if ((mode & PLL_OUTCTRL) != 0 || (opmode & PLL_RUN) != 0) {
		uint32_t have_l = rd(videoc_va + VIDEOC_PLL0 + PLL_L_VAL);
		uint32_t have_a = rd(videoc_va + VIDEOC_PLL0 + PLL_ALPHA_VAL);

		if (have_l == l && have_a == alpha) {
			say("vayu-venus: 1: PLL0 runs already at 2 x %u MHz (L %#x, alpha %#x): used, not ours\n",
			    g_rate_mhz, have_l, have_a);
			return true;
		}
		say("vayu-venus: 1: FAILED: PLL0 is somebody else's (mode %#x, L %#x, alpha %#x; wanted L %#x, alpha %#x)\n",
		    mode, have_l, have_a, l, alpha);
		return false;
	}
	g_pll0_ours = true;
	ok = put(videoc_va, "pll0 L", VIDEOC_PLL0 + PLL_L_VAL, l, ~0u) &&
	    put(videoc_va, "pll0 cal L", VIDEOC_PLL0 + PLL_CAL_L_VAL, 0x44, ~0u) &&
	    put(videoc_va, "pll0 alpha", VIDEOC_PLL0 + PLL_ALPHA_VAL, alpha, ~0u) &&
	    put(videoc_va, "pll0 config", VIDEOC_PLL0 + PLL_CONFIG_CTL, 0x20485699, ~0u) &&
	    put(videoc_va, "pll0 config U", VIDEOC_PLL0 + PLL_CONFIG_CTL_U, 0x2267, ~0u) &&
	    put(videoc_va, "pll0 config U1", VIDEOC_PLL0 + PLL_CONFIG_CTL_U1, 0x24, ~0u) &&
	    put(videoc_va, "pll0 test", VIDEOC_PLL0 + PLL_TEST_CTL, 0, ~0u) &&
	    put(videoc_va, "pll0 test U", VIDEOC_PLL0 + PLL_TEST_CTL_U, 0x2, ~0u) &&
	    put(videoc_va, "pll0 test U1", VIDEOC_PLL0 + PLL_TEST_CTL_U1, 0, ~0u) &&
	    put(videoc_va, "pll0 user U", VIDEOC_PLL0 + PLL_USER_CTL_U, 0x805, ~0u) &&
	    put(videoc_va, "pll0 user U1", VIDEOC_PLL0 + PLL_USER_CTL_U1, 0xd0, ~0u);
	if (ok) {
		mode = rd(videoc_va + VIDEOC_PLL0 + PLL_MODE);
		ok = put(videoc_va, "pll0 mode", VIDEOC_PLL0 + PLL_MODE, mode | PLL_HW_UPDATE_BYPASS,
		    PLL_HW_UPDATE_BYPASS) &&
		    put(videoc_va, "pll0 mode", VIDEOC_PLL0 + PLL_MODE,
		    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) & ~PLL_OUTCTRL, PLL_OUTCTRL) &&
		    put(videoc_va, "pll0 opmode", VIDEOC_PLL0 + PLL_OPMODE, PLL_STANDBY, ~0u) &&
		    put(videoc_va, "pll0 mode", VIDEOC_PLL0 + PLL_MODE,
		    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) | PLL_RESET_N, PLL_RESET_N);
	}
	if (ok) {
		ok = put(videoc_va, "pll0 opmode", VIDEOC_PLL0 + PLL_OPMODE, PLL_RUN, ~0u);
	}
	if (ok && !wait_bits(videoc_va, VIDEOC_PLL0 + PLL_MODE, PLL_LOCKED, true, 1000, &value)) {
		say("vayu-venus: 1: FAILED: PLL0 does not lock (mode %#x)\n", value);
		ok = false;
	}
	if (ok) {
		ok = put(videoc_va, "pll0 user", VIDEOC_PLL0 + PLL_USER_CTL,
		    rd(videoc_va + VIDEOC_PLL0 + PLL_USER_CTL) | PLL_OUT_MASK, PLL_OUT_MASK) &&
		    put(videoc_va, "pll0 mode", VIDEOC_PLL0 + PLL_MODE,
		    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) | PLL_OUTCTRL, PLL_OUTCTRL);
	}
	if (!ok) {
		return false;
	}
	say("vayu-venus: 1: PLL0 locked: 2 x %u MHz (L %#x, alpha %#x, mode %#x)\n", g_rate_mhz, l, alpha,
	    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE));
	return true;
}

static void
pll0_down(void)
{
	uint32_t value;

	if (!g_pll0_ours) {
		return;
	}
	(void)put(videoc_va, "pll0 mode", VIDEOC_PLL0 + PLL_MODE,
	    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) & ~PLL_OUTCTRL, PLL_OUTCTRL);
	(void)put(videoc_va, "pll0 user", VIDEOC_PLL0 + PLL_USER_CTL,
	    rd(videoc_va + VIDEOC_PLL0 + PLL_USER_CTL) & ~PLL_OUT_MASK, PLL_OUT_MASK);
	(void)put(videoc_va, "pll0 opmode", VIDEOC_PLL0 + PLL_OPMODE, PLL_STANDBY, ~0u);
	wr(videoc_va + VIDEOC_PLL0 + PLL_MODE, rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) | PLL_RESET_N);
	value = rd(videoc_va + VIDEOC_PLL0 + PLL_MODE);
	g_pll0_ours = false;
	say("vayu-venus: 1: PLL0 off (mode %#x)\n", value);
}

/* The iris RCG: source 1 = PLL0 main, field 3 = /2; or source 0 (XO) with the bypass field. */
static bool
iris_rcg_up(void)
{
	uint32_t found = rd(videoc_va + VIDEOC_IRIS_CFG);
	uint32_t want = (found & ~VIDEOC_CFG_MASK) | (g_xo ? 0x001u : (1u << 8 | 3u));
	uint32_t value;

	g_rcg_old = found;
	g_rcg_set = true;
	if (!put(videoc_va, "iris rcg cfg", VIDEOC_IRIS_CFG, want, VIDEOC_CFG_MASK)) {
		return false;
	}
	wr(videoc_va + VIDEOC_IRIS_CMD, rd(videoc_va + VIDEOC_IRIS_CMD) | VIDEOC_CMD_UPDATE);
	if (!wait_bits(videoc_va, VIDEOC_IRIS_CMD, VIDEOC_CMD_UPDATE, false, 500, &value)) {
		say("vayu-venus: 1: FAILED: iris RCG does not take %#x (cmd %#x)\n", want, value);
		return false;
	}
	say("vayu-venus: 1: iris RCG: cfg %#x -> %#x (source %u, field %u)%s\n", found,
	    rd(videoc_va + VIDEOC_IRIS_CFG), (rd(videoc_va + VIDEOC_IRIS_CFG) >> 8) & 7,
	    rd(videoc_va + VIDEOC_IRIS_CFG) & 0x1f, g_xo ? ", XO 19.2 MHz" : ", PLL0/2");
	return true;
}

static void
iris_rcg_down(void)
{
	uint32_t value;

	if (!g_rcg_set) {
		return;
	}
	wr(videoc_va + VIDEOC_IRIS_CFG, g_rcg_old);
	wr(videoc_va + VIDEOC_IRIS_CMD, rd(videoc_va + VIDEOC_IRIS_CMD) | VIDEOC_CMD_UPDATE);
	(void)wait_bits(videoc_va, VIDEOC_IRIS_CMD, VIDEOC_CMD_UPDATE, false, 500, &value);
	g_rcg_set = false;
	say("vayu-venus: 1: iris RCG put back: cfg %#x (cmd %#x)\n", rd(videoc_va + VIDEOC_IRIS_CFG), value);
}

/* ---- video_cc branches ---------------------------------------------------------------------------- */

struct venus_cc_branch {
	const char *name;
	uint32_t cbcr;
	bool *ours;
	bool voted;             /* BRANCH_VOTED: no clock-off status on disable */
};

static struct venus_cc_branch g_cc[] = {
	{ "video_cc_xo",        0x984, &g_br_video_cc_xo, false },     /* videocc-sm8150.c:271-283 */
	{ "video_cc_iris_ahb",  0x8f4, &g_br_iris_ahb,    true  },     /* videocc-sm8150.c:199-215 */
	{ "video_cc_mvsc_core", 0x850, &g_br_mvsc_core,   false },     /* videocc-sm8150.c:253-269 */
	{ "video_cc_mvs0_core", 0x890, &g_br_mvs0_core,   true  },     /* videocc-sm8150.c:217-233 */
	{ "video_cc_mvs1_core", 0x8d0, &g_br_mvs1_core,   true  },     /* videocc-sm8150.c:235-251 */
};

static bool
cc_branch_up(struct venus_cc_branch *b)
{
	uint32_t value = rd(videoc_va + b->cbcr);

	if ((value & 1u) != 0) {
		say("vayu-venus: 1: %s: cbcr %#x = %#x (on already, left on)\n", b->name, b->cbcr, value);
		return true;
	}
	*b->ours = true;
	wr(videoc_va + b->cbcr, value | 1u);
	if (!wait_bits(videoc_va, b->cbcr, 1u << 31, false, 500, &value)) {
		say("vayu-venus: 1: FAILED: %s does not come on (cbcr %#x = %#x)\n", b->name, b->cbcr, value);
		return false;
	}
	say("vayu-venus: 1: %s: cbcr %#x -> %#x (on)\n", b->name, b->cbcr, value);
	return true;
}

static void
cc_branch_down(struct venus_cc_branch *b)
{
	uint32_t value;

	if (!*b->ours) {
		return;
	}
	value = rd(videoc_va + b->cbcr);
	wr(videoc_va + b->cbcr, value & ~1u);
	if (b->voted) {
		delay(10);
		value = rd(videoc_va + b->cbcr);
	} else {
		(void)wait_bits(videoc_va, b->cbcr, 1u << 31, true, 500, &value);
	}
	say("vayu-venus: 1: %s off (cbcr %#x = %#x)\n", b->name, b->cbcr, value);
	*b->ours = false;
}

/* ---- the power-up and its reverse ----------------------------------------------------------------- */

static bool
blocks_map(void)
{
	gcc_va = ml_io_map(GCC_PHYS, GCC_SIZE);
	videoc_va = ml_io_map(VIDEOC_PHYS, VIDEOC_SIZE);
	venus_va = ml_io_map(VENUS_PHYS, VENUS_SIZE);
	if (gcc_va == 0 || videoc_va == 0 || venus_va == 0) {
		say("vayu-venus: 1: FAILED: GCC %#lx, video_cc %#lx or venus %#lx cannot be mapped\n",
		    (unsigned long)GCC_PHYS, (unsigned long)VIDEOC_PHYS, (unsigned long)VENUS_PHYS);
		return false;
	}
	say("vayu-venus: 1: blocks: GCC %#lx, video_cc %#lx, venus %#lx (registers only)\n",
	    (unsigned long)GCC_PHYS, (unsigned long)VIDEOC_PHYS, (unsigned long)VENUS_PHYS);
	return true;
}

static bool
venus_power_up(void)
{
	if (!blocks_map()) {
		return false;
	}
	g_power_started = true;
	if (!rails_up()) {
		return false;
	}
	if (!bcm_up()) {                        /* the NoC/DDR path the ARM9's fetch and the queues need */
		return false;
	}
	if (!gcc_video_ahb_up()) {
		return false;
	}
	if (!gdsc_up(&g_gdsc[0])) {
		return false;
	}
	if (!gdsc_up(&g_gdsc[1])) {
		return false;
	}
	if (!gdsc_up(&g_gdsc[2])) {
		return false;
	}
	if (!gcc_video_rest_up()) {
		return false;
	}
	if (!cc_branch_up(&g_cc[0])) {         /* video_cc XO, the register interface's own clock */
		return false;
	}
	if (!pll0_up()) {
		return false;
	}
	if (!iris_rcg_up()) {
		return false;
	}
	if (!cc_branch_up(&g_cc[1])) {         /* iris AHB */
		return false;
	}
	if (!cc_branch_up(&g_cc[2])) {         /* mvsc core: the ARM9's clock */
		return false;
	}
	if ((g_gdsc_mask & 2u) != 0 && !cc_branch_up(&g_cc[3])) {        /* mvs0 core */
		return false;
	}
	if ((g_gdsc_mask & 4u) != 0 && !cc_branch_up(&g_cc[4])) {        /* mvs1 core */
		return false;
	}
	venus_reset_pulse(gcc_va, "gcc video_axic", 0xb02c);
	venus_reset_pulse(videoc_va, "video_cc mvsc_core", 0x850);
	venus_reset_pulse(gcc_va, "gcc video_axi0", 0xb024);
	venus_reset_pulse(gcc_va, "gcc video_axi1", 0xb028);
	/* The firmware's line is masked before it is let out of reset: M1 polls the queue. */
	wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_MASK, 0);
	wr(venus_va + CPU_CS_BASE + CPU_CS_A2HSOFTINTCLR, 1);
	wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_CLEAR, VENUS_INTR_ALL);
	g_power_up = true;
	say("vayu-venus: 1: powered: rails, BCM%s, GCC, GDSCs %#x, PLL0 %s, iris RCG, branches; "
	    "the firmware's interrupt is masked\n", g_bcm_voted ? " (video NoC/DDR)" : "", g_gdsc_mask,
	    g_xo ? "skipped" : "at 2 x the iris rate");
	return true;
}

static void
venus_power_down(void)
{
	for (int i = (int)(sizeof(g_cc) / sizeof(g_cc[0])) - 1; i >= 0; i--) {
		cc_branch_down(&g_cc[i]);
	}
	iris_rcg_down();
	pll0_down();
	for (int i = (int)(sizeof(g_gdsc) / sizeof(g_gdsc[0])) - 1; i >= 0; i--) {
		gdsc_down(&g_gdsc[i]);
	}
	gcc_video_down();
	bcm_down();                             /* last: nothing above may touch DDR after this */
	if (g_rails_voted) {
		say("vayu-venus: 1: RPMh: mmcx.lvl/mx.lvl left at level %u: a shared rail, never voted down\n",
		    g_rail_level);
	}
	g_power_started = false;
	g_power_up = false;
}

/* ---- the apps SMMU: one free match register and one free context, our own tables ---------------- */

#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define VENUS_PTE               (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)

/*
 * The non-secure Venus context: stream 0x2300 under mask 0x60 on this phone.  The vendor
 * sm8150-vidc.dtsi:95-101 and its own comment print 0x1300, but the device tree this phone
 * actually boots with (and that the stashed recovery DT in firmware/vayu/dt has) gives
 * 0x2300 in the same bank: live.dts:25266-25271 "iommus = <0x26 0x2300 0x60>"; the other
 * three banks move with it (0x2301/0x2303/0x2304).  An SMR left for 0x1300 would never see
 * the firmware's traffic.
 */
#define VENUS_SID               0x2300          /* live.dts:25268; vendor dtsi says 0x1300 */
#define VENUS_SID_MASK          0x60
#define VENUS_IOVA              0x25800000ull   /* the venus_ns pool's first address */

/* Reads only: which match register and context are free, and whether ours is somebody's already. */
static bool
smmu_find(void)
{
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts, page, pages;

	if (g_smmu_va == 0) {
		g_smmu_va = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
		if (g_smmu_va == 0) {
			say("vayu-venus: 1: FAILED: the apps SMMU cannot be mapped at %#lx\n", (unsigned long)APPS_SMMU_PHYS);
			return false;
		}
	}
	scr0 = rd(g_smmu_va + SMMU_SCR0);
	idr0 = rd(g_smmu_va + SMMU_IDR0);
	idr1 = rd(g_smmu_va + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	say("vayu-venus: 1: smmu: scr0 %#x idr0 %#x idr1 %#x: %u match registers, %u contexts\n",
	    scr0, idr0, idr1, matches, contexts);
	if ((scr0 & (1u << 3)) != 0) {
		say("vayu-venus: 1: FAILED: the SMMU has 16-bit stream numbers (EXIDS), which this file is not written for\n");
		return false;
	}
	if (contexts > sizeof(used) / sizeof(used[0])) {
		contexts = sizeof(used) / sizeof(used[0]);
	}
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 2u << ((idr1 >> 28) & 7);
	g_smmu_match = ~0u;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(g_smmu_va + SMMU_SMR(n)), s2cr = rd(g_smmu_va + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;

		if ((smr >> 31) != 0) {
			used[s2cr & 0xff] = true;
			if (((stream ^ VENUS_SID) & ~mask) == 0) {
				say("vayu-venus: 1: FAILED: match register %u covers streams %#x/%#x already "
				    "(smr %#x, s2cr %#x): somebody's, left alone\n", n, VENUS_SID, VENUS_SID_MASK, smr, s2cr);
				return false;
			}
		} else if (g_smmu_match == ~0u) {
			g_smmu_match = n;
			g_smmu_old_smr = smr;
			g_smmu_old_s2cr = s2cr;
		}
	}
	for (g_smmu_context_n = 0; g_smmu_context_n < contexts && used[g_smmu_context_n]; g_smmu_context_n++) {
	}
	if (g_smmu_match == ~0u || g_smmu_context_n >= contexts) {
		say("vayu-venus: 1: FAILED: no match register or no context left\n");
		return false;
	}
	g_smmu_second = ml_io_map(APPS_SMMU_PHYS + page, 0x1000);
	g_smmu_context = ml_io_map(APPS_SMMU_PHYS + (uint64_t)(pages + g_smmu_context_n) * page, 0x1000);
	if (g_smmu_second == 0 || g_smmu_context == 0) {
		say("vayu-venus: 1: FAILED: match register %u and context %u cannot be mapped\n",
		    g_smmu_match, g_smmu_context_n);
		return false;
	}
	g_smmu_old_sctlr = rd(g_smmu_context + SMMU_CB_SCTLR);
	say("vayu-venus: 1: smmu: free: match register %u (smr %#x, s2cr %#x), context %u at %#lx (sctlr %#x)\n",
	    g_smmu_match, g_smmu_old_smr, g_smmu_old_s2cr, g_smmu_context_n,
	    (unsigned long)(APPS_SMMU_PHYS + (pages + g_smmu_context_n) * page), g_smmu_old_sctlr);
	if ((g_smmu_old_sctlr & 1) != 0) {
		say("vayu-venus: 1: FAILED: context %u is on although no stream uses it: somebody's, left alone\n",
		    g_smmu_context_n);
		return false;
	}
	return true;
}

/*
 * The queues' and (M2) the buffer pool's physical pages at VENUS_IOVA behind a stage-1 context
 * of ours: context first, then the match register, exactly vayu_camera.c's order
 * (vayu_camera.c:2403-2521). The two blocks are contiguous in IOVA space -- the pool starts
 * at VENUS_IOVA + VENUS_SHARED_BYTES, a 2 MB boundary -- but not in physical memory.
 */
static bool
smmu_up(vm_offset_t base, uint64_t bytes, vm_offset_t pool, uint64_t pool_bytes)
{
	uint64_t *root, *second, *third, table_root, total = bytes + pool_bytes;
	uint32_t thirds = (uint32_t)((total + 0x1fffff) >> 21), pages = (uint32_t)(total >> 12);
	uint32_t shared_pages = (uint32_t)(bytes >> 12);
	uint32_t value, sees = 1u << 31 | (uint32_t)VENUS_SID_MASK << 16 | VENUS_SID;

	if (bytes == 0 || base == 0 || (bytes & 0xfff) != 0 ||
	    (pool_bytes != 0 && (pool == 0 || (pool_bytes & 0xfff) != 0))) {
		say("vayu-venus: 1: FAILED: no queue memory to map\n");
		return false;
	}
	lck_mtx_lock(&hakios_smmu_lock);
	if (!smmu_find()) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	g_smmu_tables_bytes = round_page((vm_size_t)((2 + thirds) * 4096));
	g_smmu_tables = 0;
	if (kmem_alloc(kernel_map, &g_smmu_tables, (vm_size_t)g_smmu_tables_bytes,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("vayu-venus: 1: FAILED: no memory for the SMMU page tables\n");
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	root = (uint64_t *)g_smmu_tables;
	second = (uint64_t *)(g_smmu_tables + 4096);
	root[(VENUS_IOVA >> 30) & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		third = (uint64_t *)(g_smmu_tables + (2 + t) * 4096);

		second[((VENUS_IOVA >> 21) & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < pages; p++) {
			uint32_t index = t * 512 + p;
			vm_offset_t phys = index < shared_pages ? base + 4096ull * index :
			                   pool + 4096ull * (index - shared_pages);

			third[p] = kvtophys(phys) | VENUS_PTE;
		}
	}
	for (vm_offset_t line = g_smmu_tables; line < g_smmu_tables + (2 + thirds) * 4096; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	table_root = kvtophys(g_smmu_tables) | (uint64_t)g_smmu_context_n << 48;
	say("vayu-venus: 1: smmu: context %u gets our tables for %llu KB at %#llx%s; match register %u sends "
	    "streams %#x/%#x through it\n", g_smmu_context_n, (unsigned long long)(total >> 10),
	    (unsigned long long)VENUS_IOVA,
	    pool_bytes != 0 ? " and the buffer pool after them" : "",
	    g_smmu_match, VENUS_SID, VENUS_SID_MASK);
	if (!put(g_smmu_context, "smmu sctlr", SMMU_CB_SCTLR, 0, ~0u) ||
	    !put(g_smmu_second, "smmu cba2r", SMMU_CBA2R(g_smmu_context_n), SMMU_CBA2R_64_BIT, ~0u) ||
	    !put(g_smmu_second, "smmu cbar", SMMU_CBAR(g_smmu_context_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !put(g_smmu_context, "smmu tcr2", SMMU_CB_TCR2, SMMU_TCR2, SMMU_TCR2) ||
	    !put(g_smmu_context, "smmu tcr", SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !put(g_smmu_context, "smmu ttbr0", SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !put(g_smmu_context, "smmu ttbr0 high", SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !put(g_smmu_context, "smmu ttbr1", SMMU_CB_TTBR1, 0, ~0u) ||
	    !put(g_smmu_context, "smmu ttbr1 high", SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !put(g_smmu_context, "smmu mair0", SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) ||
	    !put(g_smmu_context, "smmu mair1", SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	wr(g_smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(g_smmu_context + SMMU_CB_TLBIASID, g_smmu_context_n);
	wr(g_smmu_context + SMMU_CB_TLBSYNC, 1);
	if (!wait_bits(g_smmu_context, SMMU_CB_TLBSTATUS, 1, false, 10000, &value)) {
		say("vayu-venus: 1: FAILED: the SMMU does not finish forgetting old translations (tlbstatus %#x)\n", value);
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (!put(g_smmu_context, "smmu sctlr", SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	g_smmu_on = true;
	if (!put(g_smmu_va, "smmu s2cr", SMMU_S2CR(g_smmu_match), g_smmu_context_n, 0x300ff) ||
	    !put(g_smmu_va, "smmu smr", SMMU_SMR(g_smmu_match), sees, ~0u)) {
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	if (rd(g_smmu_va + SMMU_SMR(g_smmu_match)) != sees ||
	    (rd(g_smmu_va + SMMU_S2CR(g_smmu_match)) & 0x300ff) != g_smmu_context_n ||
	    (rd(g_smmu_context + SMMU_CB_SCTLR) & 1) == 0 ||
	    rd(g_smmu_context + SMMU_CB_TTBR0) != (uint32_t)table_root) {
		say("vayu-venus: 1: FAILED: the SMMU did not take what was written (smr %#x s2cr %#x sctlr %#x ttbr0 %#x): "
		    "the hypervisor may own it\n", rd(g_smmu_va + SMMU_SMR(g_smmu_match)),
		    rd(g_smmu_va + SMMU_S2CR(g_smmu_match)), rd(g_smmu_context + SMMU_CB_SCTLR),
		    rd(g_smmu_context + SMMU_CB_TTBR0));
		lck_mtx_unlock(&hakios_smmu_lock);
		return false;
	}
	lck_mtx_unlock(&hakios_smmu_lock);
	say("vayu-venus: 1: smmu: streams %#x/%#x go through context %u with our page tables\n",
	    VENUS_SID, VENUS_SID_MASK, g_smmu_context_n);
	return true;
}

/*
 * What the firmware was refused, if anything: the context's FSR/FAR and the global FSR give the
 * address a failed fetch went to, which is what tells "the ARM9 read through our context" from
 * "it never got that far" (camera's smmu_faults()).  Read once and cleared.
 */
static bool
smmu_faults(void)
{
	uint32_t status, global, far_low, far_high, fsynr;

	if (!g_smmu_on) {
		return false;
	}
	status = rd(g_smmu_context + SMMU_CB_FSR);
	global = rd(g_smmu_va + SMMU_SGFSR);
	far_low = rd(g_smmu_context + SMMU_CB_FAR);
	far_high = rd(g_smmu_context + SMMU_CB_FAR + 4);
	fsynr = rd(g_smmu_context + SMMU_CB_FSYNR0);
	if ((status & SMMU_FSR_ALL) == 0 && global == 0) {
		say("vayu-venus: 1: smmu: the firmware was refused nothing (context %u, fsr 0, sgfsr 0)\n",
		    g_smmu_context_n);
		return false;
	}
	say("vayu-venus: 1: SMMU REFUSED A REQUEST: context %u status %#x address %#llx syndrome %#x; "
	    "global %#x stream %#x\n", g_smmu_context_n, status,
	    (unsigned long long)((uint64_t)far_high << 32 | far_low), fsynr, global,
	    rd(g_smmu_va + SMMU_SGFSYNR1));
	wr(g_smmu_context + SMMU_CB_FSR, SMMU_FSR_ALL);
	return true;
}

/* The match register and the context back as they were found; the tables are freed last. */
static void
smmu_down(void)
{
	uint32_t value;

	if (g_smmu_match == ~0u || g_smmu_context == 0) {
		g_smmu_match = ~0u;
		return;
	}
	(void)smmu_faults();
	lck_mtx_lock(&hakios_smmu_lock);
	wr(g_smmu_va + SMMU_SMR(g_smmu_match), g_smmu_old_smr);
	wr(g_smmu_va + SMMU_S2CR(g_smmu_match), g_smmu_old_s2cr);
	if (g_smmu_on) {
		wr(g_smmu_context + SMMU_CB_SCTLR, g_smmu_old_sctlr);
	}
	wr(g_smmu_context + SMMU_CB_TTBR0, 0);
	wr(g_smmu_context + SMMU_CB_TTBR0 + 4, 0);
	wr(g_smmu_context + SMMU_CB_TLBIASID, g_smmu_context_n);
	wr(g_smmu_context + SMMU_CB_TLBSYNC, 1);
	(void)wait_bits(g_smmu_context, SMMU_CB_TLBSTATUS, 1, false, 10000, &value);
	lck_mtx_unlock(&hakios_smmu_lock);
	g_smmu_on = false;
	g_smmu_match = ~0u;
	if (g_smmu_tables != 0) {
		kmem_free(kernel_map, g_smmu_tables, (vm_size_t)g_smmu_tables_bytes);
		g_smmu_tables = 0;
		g_smmu_tables_bytes = 0;
	}
	say("vayu-venus: 1: smmu: match register and context put back as they were found\n");
}

/* The panic path: writes only, no lock, no sleep, no free. */
static void
smmu_down_quiesce(void)
{
	if (g_smmu_match == ~0u || g_smmu_context == 0) {
		g_smmu_match = ~0u;
		return;
	}
	(void)smmu_faults();
	wr(g_smmu_va + SMMU_SMR(g_smmu_match), g_smmu_old_smr);
	wr(g_smmu_va + SMMU_S2CR(g_smmu_match), g_smmu_old_s2cr);
	wr(g_smmu_context + SMMU_CB_SCTLR, g_smmu_old_sctlr);
	wr(g_smmu_context + SMMU_CB_TTBR0, 0);
	wr(g_smmu_context + SMMU_CB_TTBR0 + 4, 0);
	wr(g_smmu_context + SMMU_CB_TLBIASID, g_smmu_context_n);
	wr(g_smmu_context + SMMU_CB_TLBSYNC, 1);
	g_smmu_on = false;
	g_smmu_match = ~0u;
}

/* ---- the HFI shared memory ------------------------------------------------------------------------ */

#define VENUS_SFR_OFF           0
#define VENUS_SFR_BYTES         0x1000
#define VENUS_TABLE_OFF         0x1000
/* The table and the queue headers are sys/vayu_venus_hfi.h's, so the host test checks these bytes. */
#define VENUS_TABLE_BYTES       VAYU_HFI_TABLE_BYTES
#define VENUS_QHDR_OFF          VAYU_HFI_QHDR_OFFSET  /* sizeof(struct hfi_queue_table_header) */
#define VENUS_QHDR_BYTES        VAYU_HFI_QHDR_BYTES   /* sizeof(struct hfi_queue_header) */
#define VENUS_QUEUE_COUNT       VAYU_HFI_IFACEQ_NUM
#define VENUS_QUEUE_BYTES       VAYU_HFI_QUEUE_BYTES  /* 1024 * 50 * 16, hfi_venus.c:36-44 */
#define VENUS_QUEUES_OFF        (VENUS_TABLE_OFF + VENUS_TABLE_BYTES)
#define VENUS_SHARED_BYTES      0x2600000       /* ALIGN(4K + table + 3 rings + 4K, 1M), hfi_venus.c:88-94 */
#define VENUS_POOL_IOVA         (VENUS_IOVA + VENUS_SHARED_BYTES)       /* 0x27e00000 */

/* The one block must hold what the firmware is told it holds. */
#if VENUS_SHARED_BYTES < (VENUS_SFR_BYTES + VENUS_TABLE_BYTES + VENUS_QUEUE_COUNT * VENUS_QUEUE_BYTES)
#error "the HFI shared block is smaller than the SFR, table and queues"
#endif
#if VENUS_POOL_IOVA != 0x27e00000ull
#error "M2's buffers are expected at 0x27e00000, right after the HFI block"
#endif

static volatile uint32_t *
shared32(uint32_t offset)
{
	return (volatile uint32_t *)(g_shared + offset);
}

static uint32_t
queue_header_off(unsigned index)
{
	return VENUS_TABLE_OFF + VENUS_QHDR_OFF + VENUS_QHDR_BYTES * index;
}

static uint32_t
queue_data_off(unsigned index)
{
	return VENUS_QUEUES_OFF + VENUS_QUEUE_BYTES * index;
}

/*
 * The one contiguous block: SFR (4 KB), the table (0xc0), the three rings.  It is kmem_alloc_contig()'d
 * (the SMMU needs physical pages), written through a VM_WIMG_WCOMB alias, and the cached alias is
 * cleaned after the allocation so no stale line can ever be written back over the firmware's queue.
 */
static bool
queues_alloc(void)
{
	volatile uint32_t *table;

	if (kmem_alloc_contig(kernel_map, &g_shared_kva, VENUS_SHARED_BYTES, PAGE_MASK, 0, 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("vayu-venus: 1: FAILED: no %u KB of contiguous memory for the HFI queues\n",
		    VENUS_SHARED_BYTES >> 10);
		g_shared_kva = 0;
		return false;
	}
	for (vm_offset_t line = g_shared_kva; line < g_shared_kva + VENUS_SHARED_BYTES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	g_shared = ml_io_map_unmappable(kvtophys(g_shared_kva), VENUS_SHARED_BYTES, VM_WIMG_WCOMB);
	if (g_shared == 0) {
		say("vayu-venus: 1: FAILED: the HFI queues cannot be mapped uncached\n");
		kmem_free(kernel_map, g_shared_kva, VENUS_SHARED_BYTES);
		g_shared_kva = 0;
		return false;
	}
	g_shared_bytes = VENUS_SHARED_BYTES;
	g_table_iova = (uint32_t)VENUS_IOVA + VENUS_TABLE_OFF;
	g_sfr_iova = (uint32_t)VENUS_IOVA + VENUS_SFR_OFF;

	/* the SFR: buf_size, then the firmware's text, hfi_cmds.h:243-246 */
	shared32(VENUS_SFR_OFF)[0] = VENUS_SFR_BYTES;

	table = shared32(VENUS_TABLE_OFF);
	vayu_hfi_table_init(table);

	for (unsigned i = 0; i < VENUS_QUEUE_COUNT; i++) {
		vayu_hfi_queue_header_init(shared32(queue_header_off(i)), i,
		    (uint32_t)VENUS_IOVA + queue_data_off(i));
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	g_queues_ready = true;
	say("vayu-venus: 1: HFI queues: %u KB of contiguous memory, phys %#llx, IOVA %#llx, table %#x, SFR %#x, "
	    "%u rings of %u KB\n", VENUS_SHARED_BYTES >> 10, (unsigned long long)kvtophys(g_shared_kva),
	    (unsigned long long)VENUS_IOVA, g_table_iova, g_sfr_iova, VENUS_QUEUE_COUNT, VENUS_QUEUE_BYTES >> 10);
	return true;
}

static void
queues_free(void)
{
	if (g_shared != 0) {
		ml_io_unmap(g_shared, g_shared_bytes);
		g_shared = 0;
	}
	if (g_shared_kva != 0) {
		kmem_free(kernel_map, g_shared_kva, g_shared_bytes);
		g_shared_kva = 0;
	}
	g_shared_bytes = 0;
	g_queues_ready = false;
}

/* ---- M2/M3's buffer pool, right after the HFI block in the same SMMU context ---------------------- */

static void
cache_clean(vm_offset_t address, uint64_t bytes)
{
	for (vm_offset_t line = address & ~(vm_offset_t)63; line < address + bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

static void
cache_invalidate(vm_offset_t address, uint64_t bytes)
{
	for (vm_offset_t line = address & ~(vm_offset_t)63; line < address + bytes; line += 64) {
		__asm__ volatile ("dc ivac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

/*
 * 64 MB first, then 48 and 32: a 1080p NV12 frame is 3 MB, and the firmware's own scratch and
 * persistent buffers (its answer to BUFFER_REQUIREMENTS) come out of the same block. If even
 * 32 MB is not contiguous the session stops with a line saying so; nothing else is touched.
 */
static bool
pool_alloc(void)
{
	static const uint64_t sizes[] = { 0x4000000, 0x3000000, 0x2000000 };

	if (g_pool_kva != 0) {
		return true;
	}
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		if (kmem_alloc_contig(kernel_map, &g_pool_kva, (vm_size_t)sizes[i], PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			g_pool_kva = 0;
			continue;
		}
		g_pool_bytes = sizes[i];
		g_pool_used = 0;
		g_pool_iova = (uint32_t)VENUS_POOL_IOVA;
		/* KMA_ZERO wrote through the cache: clean it out before the firmware may read it. */
		cache_clean(g_pool_kva, g_pool_bytes);
		say("vayu-venus: 2: buffer pool: %llu MB, phys %#llx, IOVA %#x (after the HFI block)\n",
		    (unsigned long long)(g_pool_bytes >> 20), (unsigned long long)kvtophys(g_pool_kva),
		    g_pool_iova);
		return true;
	}
	say("vayu-venus: 2: FAILED: no 32..64 MB of contiguous memory for the encoder's buffers\n");
	return false;
}

static void
pool_free(void)
{
	if (g_pool_kva != 0) {
		kmem_free(kernel_map, g_pool_kva, g_pool_bytes);
		g_pool_kva = 0;
	}
	g_pool_bytes = 0;
	g_pool_used = 0;
}

/* A slice of the pool: IOVA for the firmware, the cached address for us. */
static bool
pool_take(uint32_t bytes, uint32_t alignment, uint32_t *iova, vm_offset_t *kva)
{
	uint64_t at;

	if (g_pool_kva == 0 || bytes == 0) {
		return false;
	}
	if (alignment < 4096 || (alignment & (alignment - 1)) != 0 || alignment > 0x100000) {
		alignment = 4096;
	}
	at = (g_pool_used + alignment - 1) & ~(uint64_t)(alignment - 1);
	if (at + bytes > g_pool_bytes) {
		say("vayu-venus: 2: the pool is full: %u bytes asked, %llu of %llu used\n", bytes,
		    (unsigned long long)g_pool_used, (unsigned long long)g_pool_bytes);
		return false;
	}
	g_pool_used = at + bytes;
	if (iova != NULL) {
		*iova = g_pool_iova + (uint32_t)at;
	}
	if (kva != NULL) {
		*kva = g_pool_kva + (vm_offset_t)at;
	}
	return true;
}

/* ---- HFI packets ---------------------------------------------------------------------------------- */

/*
 * The wire format itself is sys/vayu_venus_hfi.h (builders, parsers, the queue headers), so the
 * bytes written here are the bytes tools/venus-hfi checks on the build machine.  The short names
 * below are only so this file reads like mainline's hfi code.
 */
#define HFI_CMD_SYS_INIT        VAYU_HFI_CMD_SYS_INIT
#define HFI_CMD_SYS_SET_PROPERTY VAYU_HFI_CMD_SYS_SET_PROPERTY
#define HFI_CMD_SYS_GET_PROPERTY VAYU_HFI_CMD_SYS_GET_PROPERTY
#define HFI_CMD_SYS_PING        VAYU_HFI_CMD_SYS_PING
#define HFI_MSG_SYS_INIT        VAYU_HFI_MSG_SYS_INIT
#define HFI_MSG_SYS_PROPERTY_INFO VAYU_HFI_MSG_SYS_PROPERTY_INFO
#define HFI_MSG_EVENT_NOTIFY    VAYU_HFI_MSG_EVENT_NOTIFY
#define HFI_MSG_SYS_PING_ACK    VAYU_HFI_MSG_SYS_PING_ACK
#define HFI_MSG_SYS_SESSION_INIT VAYU_HFI_MSG_SYS_SESSION_INIT
#define HFI_MSG_SESSION_LOAD_RESOURCES VAYU_HFI_MSG_SESSION_LOAD_RESOURCES
#define HFI_MSG_SESSION_START   VAYU_HFI_MSG_SESSION_START
#define HFI_MSG_SESSION_STOP    VAYU_HFI_MSG_SESSION_STOP
#define HFI_MSG_SESSION_FILL_BUFFER VAYU_HFI_MSG_SESSION_FILL_BUFFER
#define HFI_MSG_SESSION_PROPERTY_INFO VAYU_HFI_MSG_SESSION_PROPERTY_INFO
#define HFI_MSG_SESSION_RELEASE_RESOURCES VAYU_HFI_MSG_SESSION_RELEASE_RESOURCES
#define HFI_MSG_SESSION_END     VAYU_HFI_MSG_SESSION_END
#define HFI_VIDEO_ARCH_OX       VAYU_HFI_VIDEO_ARCH_OX
#define HFI_PROPERTY_SYS_DEBUG_CONFIG VAYU_HFI_PROPERTY_SYS_DEBUG_CONFIG
#define HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL VAYU_HFI_PROPERTY_SYS_CODEC_POWER_PLANE_CTRL
#define HFI_PROPERTY_SYS_IMAGE_VERSION VAYU_HFI_PROPERTY_SYS_IMAGE_VERSION
#define HFI_DEBUG_MODE_QUEUE    VAYU_HFI_DEBUG_MODE_QUEUE
#define HFI_EVENT_SYS_ERROR     VAYU_HFI_EVENT_SYS_ERROR
#define HFI_PING_COOKIE         0xbeef

static uint8_t g_packet[16384] __attribute__((aligned(8)));

static void
hfi_doorbell(void)
{
	wr(venus_va + CPU_IC_BASE + CPU_IC_SOFTINT, CPU_IC_SOFTINT_H2A);
}

static void
hfi_irq_ack(void)
{
	uint32_t status;

	wr(venus_va + CPU_CS_BASE + CPU_CS_A2HSOFTINTCLR, 1);
	status = rd(venus_va + WRAPPER_BASE + WRAPPER_INTR_STATUS);
	wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_CLEAR, status);
}

static bool
hfi_write(const void *packet, uint32_t size, bool sync)
{
	const uint32_t *src = packet;
	volatile uint32_t *qhdr = shared32(queue_header_off(0));
	uint32_t base = queue_data_off(0), dwords = size >> 2;
	uint32_t rd_idx, wr_idx, qsize, empty, first;

	if (!g_queues_ready || size < 8 || (size & 3) != 0 || dwords > VAYU_HFI_PACKET_MAX / 4) {
		say("vayu-venus: 1: HFI: a packet of %u bytes is not sent\n", size);
		return false;
	}
	rd_idx = qhdr[12];
	wr_idx = qhdr[13];
	qsize = qhdr[3];
	if (!vayu_hfi_ring_index_ok(qsize, rd_idx) || !vayu_hfi_ring_index_ok(qsize, wr_idx)) {
		say("vayu-venus: 1: HFI: the command queue's indices are bad (read %u, write %u, size %u)\n",
		    rd_idx, wr_idx, qsize);
		return false;
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	empty = vayu_hfi_ring_free(qsize, rd_idx, wr_idx);
	if (empty <= dwords) {
		qhdr[9] = 1;                    /* tx_req: it is full */
		say("vayu-venus: 1: HFI: the command queue is full (%u free, %u wanted)\n", empty, dwords);
		return false;
	}
	qhdr[9] = 0;
	first = qsize - wr_idx;
	if (first > dwords) {
		first = dwords;
	}
	for (uint32_t i = 0; i < first; i++) {
		*(volatile uint32_t *)(g_shared + base + ((wr_idx + i) << 2)) = src[i];
	}
	for (uint32_t i = first; i < dwords; i++) {
		*(volatile uint32_t *)(g_shared + base + ((i - first) << 2)) = src[i];
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	qhdr[13] = vayu_hfi_ring_advance(qsize, wr_idx, dwords);
	if (sync) {
		shared32(queue_header_off(1))[8] = 1;   /* the message queue's rx_req */
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	hfi_doorbell();
	return true;
}

/* One packet out of the message queue; 0: none. */
static uint32_t
hfi_read(void *packet, uint32_t max)
{
	volatile uint32_t *qhdr = shared32(queue_header_off(1));
	uint32_t base = queue_data_off(1);
	uint32_t *to = packet;
	uint32_t rd_idx, wr_idx, qsize, dwords, first;

	if (!g_queues_ready) {
		return 0;
	}
	rd_idx = qhdr[12];
	wr_idx = qhdr[13];
	qsize = qhdr[3];
	if (rd_idx == wr_idx || qsize == 0) {
		return 0;
	}
	if (!vayu_hfi_ring_index_ok(qsize, rd_idx) || !vayu_hfi_ring_index_ok(qsize, wr_idx)) {
		say("vayu-venus: 1: HFI: the message queue's indices are bad (read %u, write %u, size %u)\n",
		    rd_idx, wr_idx, qsize);
		return 0;
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	dwords = *(volatile uint32_t *)(g_shared + base + (rd_idx << 2)) >> 2;
	if (dwords < 2 || dwords > VAYU_HFI_MESSAGE_MAX / 4 || dwords > max / 4) {
		say("vayu-venus: 1: HFI: a bad message (dwords %u, read %u, size %u): the queue is dropped\n",
		    dwords, rd_idx, qsize);
		qhdr[12] = wr_idx;
		return 0;
	}
	first = qsize - rd_idx;
	if (first > dwords) {
		first = dwords;
	}
	for (uint32_t i = 0; i < first; i++) {
		to[i] = *(volatile uint32_t *)(g_shared + base + ((rd_idx + i) << 2));
	}
	for (uint32_t i = first; i < dwords; i++) {
		to[i] = *(volatile uint32_t *)(g_shared + base + ((i - first) << 2));
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	qhdr[12] = vayu_hfi_ring_advance(qsize, rd_idx, dwords);
	/* The firmware is asked for another interrupt only when the queue is empty (hfi_venus.c:288-303). */
	qhdr[8] = qhdr[12] == qhdr[13] ? 1u : 0u;
	if (qhdr[9] != 0) {             /* the firmware asked for the queue to be read */
		qhdr[9] = 0;
		hfi_doorbell();
	}
	__asm__ volatile ("dmb sy" : : : "memory");
	return dwords << 2;
}

static void
printable(char *to, size_t size, const uint8_t *from, uint32_t length)
{
	uint32_t i;

	if (length >= size) {
		length = (uint32_t)size - 1;
	}
	for (i = 0; i < length; i++) {
		uint8_t c = from[i];

		to[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	to[i] = 0;
}

/* The SFR, if the firmware left text there (hfi_venus.c:1034-1052). */
static void
show_sfr(void)
{
	volatile uint32_t *sfr = shared32(VENUS_SFR_OFF);
	uint32_t size = sfr[0];
	char text[200];

	if (size < 8 || size > VENUS_SFR_BYTES) {
		return;
	}
	printable(text, sizeof(text), (const uint8_t *)(g_shared + VENUS_SFR_OFF + 4), size - 4);
	say("vayu-venus: 1: SFR: \"%s\"\n", text);
}

static bool
hfi_exchange(void)
{
	uint32_t *packet = (uint32_t *)g_packet;
	uint32_t words = sizeof(g_packet) / sizeof(uint32_t);
	uint32_t size;
	bool init_done = false, init_ok = false, version_seen = false, ping_seen = false, fatal = false;
	unsigned packets = 0;

	say("vayu-venus: 1: HFI: SYS_INIT, debug %#x, power plane on, image version, ping %#x; %u ms\n",
	    g_debug, HFI_PING_COOKIE, g_wait_ms);
	if ((size = vayu_hfi_sys_init(packet, words)) == 0 || !hfi_write(packet, size, true) ||
	    (size = vayu_hfi_sys_debug_config(packet, words, g_debug)) == 0 || !hfi_write(packet, size, false) ||
	    (size = vayu_hfi_sys_codec_power_plane(packet, words, 1)) == 0 || !hfi_write(packet, size, false) ||
	    (size = vayu_hfi_sys_get_image_version(packet, words)) == 0 || !hfi_write(packet, size, true) ||
	    (size = vayu_hfi_sys_ping(packet, words, HFI_PING_COOKIE)) == 0 || !hfi_write(packet, size, true)) {
		say("vayu-venus: 1: FAILED: a command did not fit the queue\n");
		return false;
	}
	for (uint32_t ms = 0; ms < g_wait_ms; ms++) {
		while ((size = hfi_read(g_packet, sizeof(g_packet))) != 0 && packets < 64) {
			uint32_t *p = (uint32_t *)g_packet;
			uint32_t type = 0;

			packets++;
			hfi_irq_ack();
			if (!vayu_hfi_message(p, size, &type)) {
				say("vayu-venus: 1: HFI: a message whose size word does not match (%u bytes, first word %#x)\n",
				    size, p[0]);
				continue;
			}
			switch (type) {
			case HFI_MSG_SYS_INIT: {
				uint32_t error = ~0u, properties = 0;

				init_done = true;
				if (vayu_hfi_sys_init_done(p, size, &error, &properties)) {
					say("vayu-venus: 1: HFI: SYS_INIT done: error %u, %u properties, %u bytes\n",
					    error, properties, size);
					init_ok = error == 0 && properties != 0;
					for (uint32_t i = 0; i < properties && 4 + i < size / 4 && i < 8; i++) {
						say("vayu-venus: 1: HFI: SYS_INIT property word %u: %#x\n", i, p[4 + i]);
					}
				} else {
					say("vayu-venus: 1: HFI: SYS_INIT done, %u bytes (short)\n", size);
				}
				break;
			}
			case HFI_MSG_SYS_PROPERTY_INFO: {
				const uint8_t *text = NULL;
				uint32_t length = 0;

				say("vayu-venus: 1: HFI: property info: %u propert%s, property %#x\n",
				    p[2], p[2] == 1 ? "y" : "ies", size >= 16 ? p[3] : 0);
				if (vayu_hfi_image_version(p, size, &text, &length)) {
					char text_out[160];
					uint32_t show = length;

					/* The firmware's string is NUL-padded to 128 bytes: stop at the first NUL. */
					for (uint32_t i = 0; i < length; i++) {
						if (text[i] == 0) {
							show = i;
							break;
						}
					}
					printable(text_out, sizeof(text_out), text, show);
					say("vayu-venus: 1: HFI: FIRMWARE VERSION: \"%s\"\n", text_out);
					version_seen = true;
				}
				break;
			}
			case HFI_MSG_SYS_PING_ACK:
				if (size >= 12) {
					say("vayu-venus: 1: HFI: ping ack: cookie %#x%s\n", p[2],
					    vayu_hfi_ping_ack(p, size, HFI_PING_COOKIE) ? "" : " (NOT OURS)");
					ping_seen = vayu_hfi_ping_ack(p, size, HFI_PING_COOKIE);
				}
				break;
			case HFI_MSG_EVENT_NOTIFY: {
				uint32_t session = 0, event = 0, data0 = 0, data1 = 0;

				if (vayu_hfi_event(p, size, &session, &event, &data0, &data1)) {
					if (size >= 24) {
						say("vayu-venus: 1: HFI: EVENT_NOTIFY: session %#x, event %#x, data %#x/%#x\n",
						    session, event, data0, data1);
					} else {
						say("vayu-venus: 1: HFI: EVENT_NOTIFY: session %#x, event %#x, %u bytes\n",
						    session, event, size);
					}
					if (event == HFI_EVENT_SYS_ERROR) {
						fatal = true;
						show_sfr();
					}
				}
				break;
			}
			default:
				say("vayu-venus: 1: HFI: a message of type %#x, %u bytes (not handled)\n", type, size);
				break;
			}
		}
		if (init_done && (version_seen || ping_seen) && packets >= 2) {
			break;
		}
		if (fatal) {
			break;
		}
		IOSleep(1);
	}
	say("vayu-venus: 1: HFI: %u packets; SYS_INIT %s, version %s, ping %s\n", packets,
	    init_done ? (init_ok ? "OK" : "REFUSED") : "NOT SEEN", version_seen ? "SEEN" : "not seen",
	    ping_seen ? "OK" : "not seen");
	if (fatal) {
		say("vayu-venus: 1: FAILED: the firmware says SYS_ERROR\n");
		return false;
	}
	if (!init_done || !init_ok) {
		say("vayu-venus: 1: FAILED: no good SYS_INIT from the firmware\n");
		return false;
	}
	if (!version_seen) {
		say("vayu-venus: 1: the firmware did not give its version string\n");
	}
	return true;
}

/* ---- M2/M3: one session, one NV12 frame, the bitstream -------------------------------------------- */

/*
 * The session id is ours: mainline hashes the instance pointer (hash32_ptr), the firmware only
 * echoes it back. A fixed non-zero value is enough here and makes the log readable.
 */
#define HFI_SESSION_ID          0x5e551001u
#define VENUS_INPUT_MAX         4u
#define VENUS_OUTPUT_MAX        4u
#define VENUS_INTERNAL_MAX      5u
#define VENUS_INTERNAL_COUNT_MAX 4u

struct venus_buffer {
	uint32_t iova;
	vm_offset_t kva;
	uint32_t bytes;
	uint32_t type;
};

struct venus_internal_set {
	uint32_t type;
	uint32_t count;
	struct venus_buffer buffer[VENUS_INTERNAL_COUNT_MAX];
};

static uint32_t g_session = HFI_SESSION_ID;
static uint32_t g_session_frames;
static bool g_session_open, g_session_started, g_hfi_fatal;
static struct venus_buffer g_input[VENUS_INPUT_MAX], g_output[VENUS_OUTPUT_MAX];
static uint32_t g_input_n, g_output_n;
static struct venus_internal_set g_internal[VENUS_INTERNAL_MAX];
static uint32_t g_internal_n;

/*
 * Read the message queue until a packet of `want' arrives or timeout_ms passes. Everything else
 * is logged and dropped; an EVENT_NOTIFY that says SYS_ERROR or SESSION_ERROR stops the wait.
 * The firmware's interrupt stays masked in the wrapper: this is the polling path of M2.
 */
static bool
hfi_wait(uint32_t want, uint32_t timeout_ms, uint32_t *got_size)
{
	for (uint32_t ms = 0; ms < timeout_ms; ms++) {
		uint32_t size;

		while ((size = hfi_read(g_packet, sizeof(g_packet))) != 0) {
			uint32_t *p = (uint32_t *)g_packet;
			uint32_t type = 0;

			hfi_irq_ack();
			if (!vayu_hfi_message(p, size, &type)) {
				say("vayu-venus: 2: HFI: a message whose size word does not match (%u bytes, first word %#x)\n",
				    size, p[0]);
				continue;
			}
			if (type == HFI_MSG_EVENT_NOTIFY) {
				uint32_t session = 0, event = 0, data0 = 0, data1 = 0;

				if (vayu_hfi_event(p, size, &session, &event, &data0, &data1)) {
					say("vayu-venus: 2: HFI: EVENT_NOTIFY: session %#x, event %#x, data %#x/%#x\n",
					    session, event, data0, data1);
					if (event == VAYU_HFI_EVENT_SYS_ERROR ||
					    event == VAYU_HFI_EVENT_SESSION_ERROR) {
						g_hfi_fatal = true;
						show_sfr();
					}
				}
				continue;
			}
			if (type == VAYU_HFI_MSG_SESSION_EMPTY_BUFFER) {
				uint32_t session = 0, error = 0, filled = 0, buffer = 0;

				if (vayu_hfi_session_empty_done(p, size, &session, &error, &filled, &buffer)) {
					say("vayu-venus: 2: HFI: EMPTY_BUFFER done: session %#x, error %u, filled_len %u, buffer %#x\n",
					    session, error, filled, buffer);
				}
				continue;
			}
			if (type == want) {
				if (got_size != NULL) {
					*got_size = size;
				}
				return true;
			}
			say("vayu-venus: 2: HFI: while waiting for %#x: message %#x, %u bytes (dropped)\n",
			    want, type, size);
		}
		if (g_hfi_fatal) {
			return false;
		}
		IOSleep(1);
	}
	return false;
}

/* One command into the queue; `sync' asks the firmware for a prompt message (hfi_write()). */
static bool
hfi_command(const char *name, uint32_t size, bool sync)
{
	if (size == 0) {
		say("vayu-venus: 2: FAILED: %s: the packet does not fit\n", name);
		return false;
	}
	if (!hfi_write(g_packet, size, sync)) {
		say("vayu-venus: 2: FAILED: %s: the command queue refused the packet\n", name);
		return false;
	}
	return true;
}

/* A property is fire-and-forget in HFI: nothing answers it, the next message is the proof. */
static bool
hfi_property(const char *name, uint32_t property, uint32_t size)
{
	if (size == 0) {
		say("vayu-venus: 2: FAILED: SET_PROPERTY %s (%#x): the packet does not fit\n", name, property);
		return false;
	}
	if (!hfi_write(g_packet, size, false)) {
		say("vayu-venus: 2: FAILED: SET_PROPERTY %s (%#x): the command queue refused it\n", name, property);
		return false;
	}
	say("vayu-venus: 2: HFI: SET_PROPERTY %s (%#x): %u bytes\n", name, property, size);
	return true;
}

static bool
venus_session_open(void)
{
	uint32_t size, session = 0, error = ~0u, properties = 0;

	say("vayu-venus: 2: M2: SESSION_INIT: domain ENC, codec %s, session %#x\n",
	    g_codec == VAYU_HFI_CODEC_HEVC ? "HEVC" : "H.264", g_session);
	size = vayu_hfi_session_init((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_SESSION_TYPE_ENC, g_codec);
	if (!hfi_command("SESSION_INIT", size, true) ||
	    !hfi_wait(HFI_MSG_SYS_SESSION_INIT, g_wait_ms, &size)) {
		say("vayu-venus: 2: FAILED: no SESSION_INIT done in %u ms\n", g_wait_ms);
		return false;
	}
	if (!vayu_hfi_session_init_done((uint32_t *)g_packet, size, &session, &error, &properties)) {
		say("vayu-venus: 2: FAILED: SESSION_INIT done, %u bytes, malformed\n", size);
		return false;
	}
	say("vayu-venus: 2: HFI: SESSION_INIT done: session %#x, error %u, %u properties, %u bytes\n",
	    session, error, properties, size);
	if (error != 0) {
		say("vayu-venus: 2: FAILED: the firmware refused the session (error %u)\n", error);
		return false;
	}
	if (session != g_session) {
		say("vayu-venus: 2: FAILED: the firmware answered session %#x, not ours %#x\n", session, g_session);
		return false;
	}
	g_session_open = true;
	return true;
}

static bool
venus_session_properties(void)
{
	uint32_t packet_words = sizeof(g_packet) / 4;
	const uint32_t stride[6] = { VAYU_HFI_BUFFER_INPUT, 2, g_width, g_height, g_width, g_height / 2 };
	uint32_t max_bitrate = g_bitrate > 0x7fffffffu / 2 ? g_bitrate : g_bitrate * 2;

	if (!hfi_property("plane actual info (stride)", VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO,
	    vayu_hfi_session_property((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO, stride, 6)) ||
	    !hfi_property("FRAME_SIZE input", VAYU_HFI_PROPERTY_PARAM_FRAME_SIZE,
	    vayu_hfi_session_frame_size((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_BUFFER_INPUT, g_width, g_height)) ||
	    !hfi_property("FRAME_SIZE output", VAYU_HFI_PROPERTY_PARAM_FRAME_SIZE,
	    vayu_hfi_session_frame_size((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_BUFFER_OUTPUT, g_width, g_height)) ||
	    !hfi_property("color format NV12", VAYU_HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT,
	    vayu_hfi_session_color_format((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_BUFFER_INPUT, VAYU_HFI_COLOR_FORMAT_NV12)) ||
	    !hfi_property("work mode 2", VAYU_HFI_PROPERTY_PARAM_WORK_MODE,
	    vayu_hfi_session_scalar((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_PROPERTY_PARAM_WORK_MODE, VAYU_HFI_WORK_MODE_2)) ||
	    !hfi_property("frame rate", VAYU_HFI_PROPERTY_CONFIG_FRAME_RATE,
	    vayu_hfi_session_frame_rate((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_BUFFER_OUTPUT, g_fps))) {
		return false;
	}
	if (g_codec == VAYU_HFI_CODEC_HEVC &&
	    !hfi_property("profile/level", VAYU_HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT,
	    vayu_hfi_session_profile_level((uint32_t *)g_packet, packet_words, g_session,
	    VAYU_HFI_HEVC_PROFILE_MAIN, VAYU_HFI_HEVC_LEVEL_51))) {
		return false;
	}
	return hfi_property("rate control VBR_CFR", VAYU_HFI_PROPERTY_PARAM_VENC_RATE_CONTROL,
	           vayu_hfi_session_scalar((uint32_t *)g_packet, packet_words, g_session,
	           VAYU_HFI_PROPERTY_PARAM_VENC_RATE_CONTROL, VAYU_HFI_RATE_CONTROL_VBR_CFR)) &&
	       hfi_property("target bitrate", VAYU_HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE,
	           vayu_hfi_session_bitrate((uint32_t *)g_packet, packet_words, g_session,
	           VAYU_HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE, g_bitrate, 0)) &&
	       hfi_property("maximum bitrate", VAYU_HFI_PROPERTY_CONFIG_VENC_MAX_BITRATE,
	           vayu_hfi_session_bitrate((uint32_t *)g_packet, packet_words, g_session,
	           VAYU_HFI_PROPERTY_CONFIG_VENC_MAX_BITRATE, max_bitrate, 0)) &&
	       hfi_property("sequence header with frame", VAYU_HFI_PROPERTY_CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER,
	           vayu_hfi_session_scalar((uint32_t *)g_packet, packet_words, g_session,
	           VAYU_HFI_PROPERTY_CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER, 1));
}

/*
 * BUFFER_REQUIREMENTS: GET_PROPERTY 0x202001, answered by SESSION_PROPERTY_INFO 0x221009 with
 * one eight-word record per buffer type (hfi_helper.h:1173-1181; hfi_msgs.c:430-460).
 */
static bool
venus_buffer_requirements(struct vayu_hfi_buffer_requirement *req)
{
	uint32_t size, session = 0, property = 0, words = 0, records;
	const uint32_t *data = NULL;

	if (!hfi_command("GET BUFFER_REQUIREMENTS",
	    vayu_hfi_session_get_property((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS), true) ||
	    !hfi_wait(HFI_MSG_SESSION_PROPERTY_INFO, g_wait_ms, &size)) {
		say("vayu-venus: 2: FAILED: no BUFFER_REQUIREMENTS answer in %u ms\n", g_wait_ms);
		return false;
	}
	if (!vayu_hfi_session_property_info((uint32_t *)g_packet, size, &session, &property,
	    &data, &words)) {
		say("vayu-venus: 2: FAILED: BUFFER_REQUIREMENTS answer, %u bytes, malformed\n", size);
		return false;
	}
	if (session != g_session || property != VAYU_HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS) {
		say("vayu-venus: 2: FAILED: property info for session %#x property %#x, not the buffer requirements\n",
		    session, property);
		return false;
	}
	records = words / VAYU_HFI_BUFFER_REQUIREMENT_WORDS;
	say("vayu-venus: 2: HFI: BUFFER_REQUIREMENTS: %u records, %u bytes\n", records, size);
	for (uint32_t i = 0; i < records; i++) {
		struct vayu_hfi_buffer_requirement one;

		if (!vayu_hfi_buffer_requirement(data, words, i, &one)) {
			break;
		}
		if (one.type >= VAYU_HFI_BUFFER_TYPE_MAX) {
			continue;
		}
		req[one.type] = one;
		say("vayu-venus: 2: HFI: buffer type %u: size %u, region %u, min %u, actual %u, "
		    "contiguous %u, alignment %u\n", one.type, one.size, one.region_size,
		    one.count_min, one.count_actual, one.contiguous, one.alignment);
	}
	return records != 0;
}

static uint32_t
venus_align_up(uint32_t value, uint32_t alignment)
{
	if (alignment < 4096 || (alignment & (alignment - 1)) != 0 || alignment > 0x100000) {
		alignment = 4096;
	}
	return (value + alignment - 1) & ~(alignment - 1);
}

static bool
venus_input_and_output(const struct vayu_hfi_buffer_requirement *req)
{
	uint32_t addresses[VENUS_OUTPUT_MAX], frame = g_width * g_height * 3 / 2;
	uint32_t in_size = req[VAYU_HFI_BUFFER_INPUT].size, out_size = req[VAYU_HFI_BUFFER_OUTPUT].size;
	uint32_t in_count = req[VAYU_HFI_BUFFER_INPUT].count_actual;
	uint32_t out_count = req[VAYU_HFI_BUFFER_OUTPUT].count_actual;

	if (in_size < frame) {
		in_size = frame;
	}
	in_size = venus_align_up(in_size, req[VAYU_HFI_BUFFER_INPUT].alignment);
	if (out_size < 0x100000) {
		out_size = 0x100000;
	}
	out_size = venus_align_up(out_size, req[VAYU_HFI_BUFFER_OUTPUT].alignment);
	in_count = in_count == 0 ? 2 : in_count;
	out_count = out_count == 0 ? 2 : out_count;
	if (in_count > VENUS_INPUT_MAX) {
		in_count = VENUS_INPUT_MAX;
	}
	if (out_count > VENUS_OUTPUT_MAX) {
		out_count = VENUS_OUTPUT_MAX;
	}
	for (g_input_n = 0; g_input_n < in_count; g_input_n++) {
		if (!pool_take(in_size, req[VAYU_HFI_BUFFER_INPUT].alignment,
		    &g_input[g_input_n].iova, &g_input[g_input_n].kva)) {
			return false;
		}
		g_input[g_input_n].bytes = in_size;
		g_input[g_input_n].type = VAYU_HFI_BUFFER_INPUT;
		addresses[g_input_n] = g_input[g_input_n].iova;
	}
	if (!hfi_command("SET_BUFFERS input",
	    vayu_hfi_session_set_buffers((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_BUFFER_INPUT, in_size, addresses, g_input_n), true)) {
		return false;
	}
	say("vayu-venus: 2: HFI: %u input buffers of %u bytes at %#x (%ux%u NV12)\n",
	    g_input_n, in_size, g_input[0].iova, g_width, g_height);
	for (g_output_n = 0; g_output_n < out_count; g_output_n++) {
		if (!pool_take(out_size, req[VAYU_HFI_BUFFER_OUTPUT].alignment,
		    &g_output[g_output_n].iova, &g_output[g_output_n].kva)) {
			return false;
		}
		g_output[g_output_n].bytes = out_size;
		g_output[g_output_n].type = VAYU_HFI_BUFFER_OUTPUT;
		addresses[g_output_n] = g_output[g_output_n].iova;
	}
	if (!hfi_command("SET_BUFFERS output",
	    vayu_hfi_session_set_buffers((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_BUFFER_OUTPUT, out_size, addresses, g_output_n), true)) {
		return false;
	}
	say("vayu-venus: 2: HFI: %u output buffers of %u bytes at %#x\n", g_output_n, out_size,
	    g_output[0].iova);
	return true;
}

/* The firmware's own scratch/persistent buffers: one SET_BUFFERS per buffer, as mainline does. */
static bool
venus_internal_buffers(const struct vayu_hfi_buffer_requirement *req)
{
	static const uint32_t types[VENUS_INTERNAL_MAX] = {
		VAYU_HFI_BUFFER_INTERNAL_SCRATCH, VAYU_HFI_BUFFER_INTERNAL_SCRATCH_1,
		VAYU_HFI_BUFFER_INTERNAL_SCRATCH_2, VAYU_HFI_BUFFER_INTERNAL_PERSIST,
		VAYU_HFI_BUFFER_INTERNAL_PERSIST_1,
	};

	g_internal_n = 0;
	for (uint32_t t = 0; t < VENUS_INTERNAL_MAX; t++) {
		struct venus_internal_set *set = &g_internal[g_internal_n];
		uint32_t type = types[t], count = req[type].count_actual;

		if (req[type].size == 0 || count == 0) {
			continue;
		}
		if (count > VENUS_INTERNAL_COUNT_MAX) {
			count = VENUS_INTERNAL_COUNT_MAX;
		}
		set->type = type;
		set->count = 0;
		for (uint32_t i = 0; i < count; i++) {
			uint32_t address;

			if (!pool_take(req[type].size, req[type].alignment, &address,
			    &set->buffer[i].kva)) {
				return false;
			}
			set->buffer[i].iova = address;
			set->buffer[i].bytes = req[type].size;
			set->buffer[i].type = type;
			if (!hfi_command("SET_BUFFERS internal",
			    vayu_hfi_session_set_buffers((uint32_t *)g_packet, sizeof(g_packet) / 4,
			    g_session, type, req[type].size, &address, 1), true)) {
				return false;
			}
			set->count++;
		}
		say("vayu-venus: 2: HFI: %u internal buffers of type %u, %u bytes each\n", set->count,
		    type, req[type].size);
		g_internal_n++;
	}
	return true;
}

static bool
venus_session_start(void)
{
	uint32_t size, session = 0, error = ~0u;

	if (!hfi_command("LOAD_RESOURCES",
	    vayu_hfi_session_command((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_CMD_SESSION_LOAD_RESOURCES), true) ||
	    !hfi_wait(HFI_MSG_SESSION_LOAD_RESOURCES, g_wait_ms, &size)) {
		say("vayu-venus: 2: FAILED: no LOAD_RESOURCES done in %u ms\n", g_wait_ms);
		return false;
	}
	if (!vayu_hfi_session_done((uint32_t *)g_packet, size, HFI_MSG_SESSION_LOAD_RESOURCES,
	    &session, &error)) {
		say("vayu-venus: 2: FAILED: LOAD_RESOURCES done, %u bytes, malformed\n", size);
		return false;
	}
	say("vayu-venus: 2: HFI: LOAD_RESOURCES done: error %u\n", error);
	if (error != 0 || !hfi_command("START",
	    vayu_hfi_session_command((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    VAYU_HFI_CMD_SESSION_START), true) ||
	    !hfi_wait(HFI_MSG_SESSION_START, g_wait_ms, &size)) {
		say("vayu-venus: 2: FAILED: the session did not start (error %u)\n", error);
		return false;
	}
	if (!vayu_hfi_session_done((uint32_t *)g_packet, size, HFI_MSG_SESSION_START, &session,
	    &error) || error != 0) {
		say("vayu-venus: 2: FAILED: START done, %u bytes, error %u\n", size, error);
		return false;
	}
	say("vayu-venus: 2: HFI: START done: error %u\n", error);
	g_session_started = true;
	return true;
}

/* A horizontal luma ramp with a slight vertical tilt; chroma neutral at 128 (the NV12 layout). */
static void
venus_fill_gradient(const struct venus_buffer *buffer, uint32_t frame)
{
	uint8_t *y = (uint8_t *)buffer->kva;
	uint8_t *uv = y + g_width * g_height;
	uint32_t chroma_height = g_height / 2;

	for (uint32_t row = 0; row < g_height; row++) {
		uint32_t tilt = row * 16 / g_height;
		uint8_t *line = y + (uint64_t)row * g_width;

		for (uint32_t column = 0; column < g_width; column++) {
			uint32_t value = 16 + (column * 219) / (g_width - 1) + tilt;

			line[column] = (uint8_t)(value > 235 ? 235 : value);
		}
	}
	memset(uv, 128, g_width * chroma_height);
	cache_clean(buffer->kva, frame);
}

static const char *
venus_nal_name(uint32_t type, bool hevc)
{
	if (hevc) {
		switch (type) {
		case 32: return "VPS";
		case 33: return "SPS";
		case 34: return "PPS";
		case 19:
		case 20: return "IDR";
		case 21: return "CRA";
		case 39: return "SEI";
		case 1: return "TRAIL_R";
		default: return NULL;
		}
	}
	switch (type) {
	case 7: return "SPS";
	case 8: return "PPS";
	case 5: return "IDR";
	case 6: return "SEI";
	case 1: return "non-IDR";
	default: return NULL;
	}
}

/* The Annex-B start codes, and the NAL unit type behind each: the first six, space separated. */
static void
venus_nal_scan(char *out, size_t out_size, const uint8_t *data, uint32_t length)
{
	bool hevc = g_codec == VAYU_HFI_CODEC_HEVC;
	uint32_t found = 0;

	out[0] = 0;
	for (uint32_t i = 0; i + 4 < length && found < 6; i++) {
		uint32_t at, type;
		const char *name;
		size_t used;

		if (data[i] != 0 || data[i + 1] != 0 || data[i + 2] != 1) {
			continue;
		}
		at = i + 3;
		if (at + (hevc ? 1u : 0u) >= length) {
			break;
		}
		type = hevc ? (uint32_t)((data[at] >> 1) & 0x3f) : (uint32_t)(data[at] & 0x1f);
		name = venus_nal_name(type, hevc);
		i = at;
		if (name == NULL) {
			continue;
		}
		used = strlen(out);
		if (used + 2 < out_size) {
			snprintf(out + used, out_size - used, "%s%s", found == 0 ? "" : " ", name);
			found++;
		}
	}
	if (found == 0) {
		snprintf(out, out_size, "none");
	}
}

static void
venus_hex_line(char *out, size_t out_size, const uint8_t *data, uint32_t length)
{
	uint32_t bytes = length > 64 ? 64 : length, at = 0;

	for (uint32_t i = 0; i < bytes && at + 3 < out_size; i++) {
		at += (uint32_t)snprintf(out + at, out_size - at, "%02x", data[i]);
	}
	out[at] = 0;
}

/* Write the bitstream where the lead can pull it; the first path that opens wins. */
static void
venus_dump_bitstream(const uint8_t *data, uint32_t length)
{
	static const char *const paths[] = {
		"/private/var/tmp/venus_out.h265", "/var/tmp/venus_out.h265", "/venus_out.h265",
	};
	vfs_context_t context = vfs_context_kernel();

	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		vnode_t vnode = NULLVP;
		int written = 0, error;

		error = vnode_open(paths[i], O_CREAT | FWRITE | O_TRUNC, 0644, 0, &vnode, context);
		if (error != 0) {
			continue;
		}
		error = vn_rdwr(UIO_WRITE, vnode, (caddr_t)data, (int)length, 0, UIO_SYSSPACE,
		    IO_UNIT | IO_SYNC, vfs_context_ucred(context), &written, vfs_context_proc(context));
		vnode_close(vnode, FWRITE, context);
		if (error == 0 && written == (int)length) {
			say("vayu-venus: 2: wrote %u bytes to %s\n", length, paths[i]);
			return;
		}
		say("vayu-venus: 2: %s: write error %d, %d of %u bytes\n", paths[i], error, written,
		    length);
	}
	say("vayu-venus: 2: the bitstream could not be written to /private/var/tmp, /var/tmp or /\n");
}

/* The one frame: FTB on every output buffer, then ETB with the gradient. */
static bool
venus_encode_frame(void)
{
	uint32_t size;
	uint32_t frame = g_width * g_height * 3 / 2;
	struct vayu_hfi_fill_done fbd;
	struct venus_buffer *output = NULL;
	uint8_t *bits;
	char line[3 * 64 + 1], nals[64];

	for (uint32_t i = 0; i < g_output_n; i++) {
		if (!hfi_command("FILL_BUFFER",
		    vayu_hfi_session_fill_buffer((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
		    0, g_output[i].iova, g_output[i].bytes, i), true)) {
			return false;
		}
	}
	say("vayu-venus: 2: HFI: %u FILL_BUFFER commands; the output buffers are the firmware's\n", g_output_n);
	venus_fill_gradient(&g_input[0], frame);
	if (!hfi_command("EMPTY_BUFFER",
	    vayu_hfi_session_empty_buffer((uint32_t *)g_packet, sizeof(g_packet) / 4, g_session,
	    g_input[0].iova, g_input[0].bytes, frame, 0), true)) {
		return false;
	}
	say("vayu-venus: 2: HFI: EMPTY_BUFFER: %u bytes of NV12 at %#x, %ux%u, %u fps, %u bit/s\n",
	    frame, g_input[0].iova, g_width, g_height, g_fps, g_bitrate);
	if (!hfi_wait(HFI_MSG_SESSION_FILL_BUFFER, g_frame_ms, &size)) {
		say("vayu-venus: 2: FAILED: no FILL_BUFFER done in %u ms\n", g_frame_ms);
		return false;
	}
	if (!vayu_hfi_session_fill_done((uint32_t *)g_packet, size, &fbd)) {
		say("vayu-venus: 2: FAILED: FILL_BUFFER done, %u bytes, malformed\n", size);
		return false;
	}
	for (uint32_t i = 0; i < g_output_n; i++) {
		if (g_output[i].iova == fbd.packet_buffer) {
			output = &g_output[i];
			break;
		}
	}
	if (output == NULL) {
		say("vayu-venus: 2: FAILED: FILL_BUFFER done names buffer %#x, not one of ours\n",
		    fbd.packet_buffer);
		return false;
	}
	if (fbd.error != 0) {
		say("vayu-venus: 2: FAILED: FILL_BUFFER done: error %u, filled_len %u\n", fbd.error,
		    fbd.filled_len);
		return false;
	}
	if (fbd.offset >= output->bytes || fbd.filled_len > output->bytes - fbd.offset) {
		say("vayu-venus: 2: FAILED: FILL_BUFFER done: offset %u + length %u past the %u-byte buffer\n",
		    fbd.offset, fbd.filled_len, output->bytes);
		return false;
	}
	bits = (uint8_t *)(output->kva + fbd.offset);
	cache_invalidate(output->kva, output->bytes);
	venus_nal_scan(nals, sizeof(nals), bits, fbd.filled_len);
	venus_hex_line(line, sizeof(line), bits, fbd.filled_len);
	say("vayu-venus: 2: HFI: FILL_BUFFER_DONE flags=%#x filled_len=%u (nal: %s), smmu_faults=%u\n",
	    fbd.flags, fbd.filled_len, nals, smmu_faults() ? 1u : 0u);
	say("vayu-venus: 2: bitstream[0..%u] %s\n", fbd.filled_len > 64 ? 64 : fbd.filled_len, line);
	if (fbd.filled_len >= 4) {
		say("vayu-venus: 2: bitstream starts %02x %02x %02x %02x; picture type %#x, tag %u\n",
		    bits[0], bits[1], bits[2], bits[3], fbd.picture_type, fbd.output_tag);
	}
	if (g_dump) {
		venus_dump_bitstream(bits, fbd.filled_len);
	}
	return fbd.filled_len != 0;
}

static void
venus_session_close(void)
{
	uint32_t size, session = 0, error = 0;

	if (g_session_started) {
		if (hfi_command("STOP", vayu_hfi_session_command((uint32_t *)g_packet,
		    sizeof(g_packet) / 4, g_session, VAYU_HFI_CMD_SESSION_STOP), true) &&
		    hfi_wait(HFI_MSG_SESSION_STOP, 1000, &size) &&
		    vayu_hfi_session_done((uint32_t *)g_packet, size, HFI_MSG_SESSION_STOP, &session,
		    &error)) {
			say("vayu-venus: 2: HFI: STOP done: error %u\n", error);
		}
		g_session_started = false;
	}
	if (g_session_open) {
		(void)hfi_command("RELEASE_RESOURCES", vayu_hfi_session_command((uint32_t *)g_packet,
		    sizeof(g_packet) / 4, g_session, VAYU_HFI_CMD_SESSION_RELEASE_RESOURCES), true);
		(void)hfi_wait(HFI_MSG_SESSION_RELEASE_RESOURCES, 1000, &size);
		(void)hfi_command("SESSION_END", vayu_hfi_session_command((uint32_t *)g_packet,
		    sizeof(g_packet) / 4, g_session, VAYU_HFI_CMD_SYS_SESSION_END), true);
		(void)hfi_wait(HFI_MSG_SESSION_END, 1000, &size);
		g_session_open = false;
	}
}

static bool
step_m2(void)
{
	struct vayu_hfi_buffer_requirement requirements[VAYU_HFI_BUFFER_TYPE_MAX] = { 0 };

	say("vayu-venus: 2: M2/M3: one %ux%u NV12 frame (%u bytes) to %s, bitrate %u bit/s\n",
	    g_width, g_height, g_width * g_height * 3 / 2,
	    g_codec == VAYU_HFI_CODEC_HEVC ? "HEVC" : "H.264", g_bitrate);
	if (!venus_session_open()) {
		return false;
	}
	if (!venus_session_properties()) {
		say("vayu-venus: 2: FAILED: a session property did not fit the queue\n");
		return false;
	}
	if (!venus_buffer_requirements(requirements)) {
		say("vayu-venus: 2: FAILED: no buffer requirements: the firmware will not be given guessed sizes\n");
		return false;
	}
	if (!venus_input_and_output(requirements) || !venus_internal_buffers(requirements)) {
		say("vayu-venus: 2: FAILED: the buffers do not fit the pool (or a SET_BUFFERS was refused)\n");
		return false;
	}
	if (!venus_session_start()) {
		return false;
	}
	if (!venus_encode_frame()) {
		return false;
	}
	g_session_frames++;
	venus_session_close();
	return true;
}

static bool
venus_run(void)
{
	uint32_t arg0 = 0, version;

	if (!put(venus_va + CPU_CS_BASE, "UC_REGION_ADDR", UC_REGION_ADDR, g_table_iova, ~0u) ||
	    !put(venus_va + CPU_CS_BASE, "UC_REGION_SIZE", UC_REGION_SIZE, VENUS_SHARED_BYTES, ~0u) ||
	    !put(venus_va + CPU_CS_BASE, "CPU_CS_SCIACMDARG2", CPU_CS_SCIACMDARG2, g_table_iova, ~0u) ||
	    !put(venus_va + CPU_CS_BASE, "CPU_CS_SCIACMDARG1", CPU_CS_SCIACMDARG1, 1, ~0u) ||
	    !put(venus_va + CPU_CS_BASE, "SFR_ADDR", SFR_ADDR, g_sfr_iova, ~0u)) {
		say("vayu-venus: 1: FAILED: the firmware's registers did not take what was written\n");
		return false;
	}
	say("vayu-venus: 1: registers: UC region %#x/%#x, table %#x, SFR %#x (read back); VIDC_CTRL_INIT\n",
	    g_table_iova, VENUS_SHARED_BYTES, g_table_iova, g_sfr_iova);
	wr(venus_va + CPU_CS_BASE + VIDC_CTRL_INIT, 1);
	for (unsigned i = 0; i < 100; i++) {
		arg0 = rd(venus_va + CPU_CS_BASE + CPU_CS_SCIACMDARG0);
		if (arg0 != 0) {
			break;
		}
		IOSleep(1);
	}
	if (arg0 == 0 || arg0 == 0xffffffffu) {
		say("vayu-venus: 1: FAILED: the firmware did not answer VIDC_CTRL_INIT (arg0 %#x after 100 ms: "
		    "no clock, no power, or nothing behind the bus)\n", arg0);
		return false;
	}
	if ((arg0 & 0xfe) == 4) {
		say("vayu-venus: 1: FAILED: the firmware says the UC region is invalid (arg0 %#x)\n", arg0);
		return false;
	}
	version = rd(venus_va + WRAPPER_BASE + WRAPPER_HW_VERSION);
	say("vayu-venus: 1: the firmware took VIDC_CTRL_INIT (arg0 %#x); wrapper version %u.%u.%u%s\n", arg0,
	    (version >> 28) & 0xf, (version >> 16) & 0xfff, version & 0xffff,
	    version == 0xffffffffu ? " (ALL ONES: the block may not be powered)" : "");
	return true;
}

/* ---- TrustZone: the one PAS step vayu_pas.c does not export --------------------------------------- */

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)          /* vayu_pas.c:1132 */
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)          /* vayu_pas.c:1133 */
#define SCM_BOOT_SET_REMOTE_STATE SCM_CALL(1, 0x0a)     /* qcom_scm.h:80, :85 */

static uint64_t g_scm_convention = SCM_64;

/* The retry rule and the one shared lock are vayu_scm.c's; on the panic path the lock is not taken. */
static int32_t
venus_scm(uint64_t convention, uint64_t command, uint64_t arguments, uint64_t first, uint64_t second,
    uint64_t third, uint64_t *answer)
{
	uint64_t function = convention | SCM_SIP | command;

	/*
	 * vayu_pas.c's retry rule lives in vayu_scm.c now, with the one lock every TrustZone call
	 * takes.  On the panic path (vayu_venus_quiesce() -> vayu_scm_quiescing()) the call is made
	 * without that lock: this is the one place that must not sleep or wait.
	 */
	return vayu_scm_call(function, arguments, first, second, third, 0, answer);
}

/*
 * The image is in place (vayu_pas_load()).  Ask TrustZone to authenticate and release it; if the
 * 64-bit convention answers "not available", try the 32-bit one, as vayu_pas.c does once.
 */
static bool
auth_and_reset(void)
{
	uint64_t answer = ~0ull;
	int32_t result;

	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint64_t convention = attempt == 0 ? SCM_64 : 0;

		result = venus_scm(convention, SCM_PAS_AUTH_AND_RESET, 1, venus.pas_id, 0, 0, &answer);
		say("vayu-venus: 1: TrustZone AUTH_AND_RESET (image %u, %s): %d, answer %#llx\n",
		    venus.pas_id, convention == SCM_64 ? "SMC64" : "SMC32", result, answer);
		if (result == 0 && answer == 0) {
			g_scm_convention = convention;
			return true;
		}
		if (result != -1) {             /* a real refusal; the other convention will not help */
			break;
		}
	}
	say("vayu-venus: 1: FAILED: TrustZone does not authenticate image %u\n", venus.pas_id);
	return false;
}

static bool
firmware_up(void)
{
	uint64_t answer = ~0ull;
	int32_t result;

	if (!vayu_pas_supported(&venus)) {
		say("vayu-venus: 1: FAILED: TrustZone does not answer IS_CALL_AVAILABLE either way\n");
		return false;
	}
	if (!vayu_pas_load(&venus)) {
		say("vayu-venus: 1: FAILED: Venus's image is not loaded\n");
		return false;
	}
	if (!auth_and_reset()) {
		return false;
	}
	g_firmware_up = true;
	/* mainline venus_resume(): qcom_scm_set_remote_state(1, 0), its EINVAL ignored (firmware.c:61-67) */
	result = venus_scm(g_scm_convention, SCM_BOOT_SET_REMOTE_STATE, 2, 1, 0, 0, &answer);
	say("vayu-venus: 1: TrustZone SET_REMOTE_STATE(1): %d, answer %#llx%s\n", result, answer,
	    result == 0 ? "" : " (ignored, as mainline ignores -EINVAL)");
	return true;
}

static void
firmware_down(void)
{
	uint64_t answer = ~0ull;
	int32_t result;

	if (!g_firmware_up) {
		return;
	}
	g_firmware_up = false;
	result = venus_scm(g_scm_convention, SCM_PAS_SHUTDOWN, 1, venus.pas_id, 0, 0, &answer);
	if (result != 0 || answer != 0) {
		say("vayu-venus: 1: TRUSTZONE DOES NOT SHUT VENUS DOWN: %d, answer %#llx\n", result, answer);
	} else {
		say("vayu-venus: 1: TrustZone shut Venus down\n");
	}
}

/* ---- M1 ----------------------------------------------------------------------------------------- */

static struct vayu_rproc venus = {
	.name = "venus", .firmware = "venus",           /* dt: qcom,firmware-name = "venus" (sm8150.dtsi:1936) */
	.pas_id = 9,                                    /* dt: qcom,pas-id = <9> (sm8150.dtsi:1927); firmware.c:24 */
	.memory = 0x97600000, .memory_size = 0x500000,  /* dt: pil_video_region@97600000, 0x500000 (sm8150.dtsi:639-643) */
	/* Venus has no SMP2P item and no GLINK edge: the markers below can never match one. */
	.smem_host = 0xff, .smp2p_theirs = 0xffff, .smp2p_ours = 0xffff,
	.crash_item = 0, .apcs_smp2p = 0,
	.rails = { NULL, NULL }, .rail_level = 0, .rails_shared = false,
};

static bool
step_m1(void)
{
	say("vayu-venus: 1: M1: power, venus.mdt through PAS, HFI queues behind our SMMU, SYS_INIT; "
	    "every step is logged\n");
	if (!venus_power_up()) {
		say("vayu-venus: 1: FAILED: power\n");
		return false;
	}
	if (!queues_alloc()) {
		say("vayu-venus: 1: FAILED: queues\n");
		return false;
	}
	if (g_level >= 2 && !pool_alloc()) {
		say("vayu-venus: 1: FAILED: buffer pool\n");
		return false;
	}
	if (!smmu_up(g_shared_kva, g_shared_bytes, g_pool_kva, g_pool_bytes)) {
		say("vayu-venus: 1: FAILED: SMMU\n");
		return false;
	}
	if (!firmware_up()) {
		say("vayu-venus: 1: FAILED: firmware\n");
		return false;
	}
	if (!venus_run()) {
		say("vayu-venus: 1: FAILED: registers/VIDC_CTRL_INIT\n");
		return false;
	}
	if (!hfi_exchange()) {
		say("vayu-venus: 1: FAILED: HFI\n");
		return false;
	}
	say("vayu-venus: 1: PASS\n");
	if (g_level >= 2) {
		if (step_m2()) {
			say("vayu-venus: 2: PASS: one NV12 frame became a bitstream, %u frames\n",
			    g_session_frames);
		} else {
			say("vayu-venus: 2: FAILED: M2/M3 did not finish; M1's lines above are the proof it ran\n");
			venus_session_close();
		}
	}
	if (g_secs != 0) {
		say("vayu-venus: 1: M1 OK; kept up for %u s more, for late messages\n", g_secs);
		for (uint32_t ms = 0; ms < g_secs * 1000; ms++) {
			uint32_t size;

			while ((size = hfi_read(g_packet, sizeof(g_packet))) != 0) {
				uint32_t *p = (uint32_t *)g_packet;

				hfi_irq_ack();
				if (size >= 8) {
					say("vayu-venus: 1: +%u ms: message %#x, %u bytes\n", ms, p[1], size);
				}
			}
			IOSleep(1);
		}
	}
	say("vayu-venus: 1: M1 done: the firmware booted, answered SYS_INIT, and is taken down now\n");
	return true;
}

static void
venus_down(void)
{
	if (g_power_up && venus_va != 0) {
		wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_MASK, 0);
		hfi_irq_ack();
	}
	firmware_down();
	smmu_down();
	queues_free();
	pool_free();
	if (videoc_va != 0) {
		venus_power_down();
	}
	say("vayu-venus: 1: everything taken down\n");
}

/* ---- the panic path ------------------------------------------------------------------------------- */

void
vayu_venus_quiesce(void)
{
	char line[160];

	if (!g_firmware_up && !g_smmu_on && !g_power_started) {
		return;
	}
	g_leaving = true;
	vayu_scm_quiescing();           /* the shutdown below is asked without the shared lock (vayu_scm.c) */
	loader_log_line("vayu-venus: quiesce: the kernel is on its way back to the loader");
	if (g_power_up && venus_va != 0) {
		wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_MASK, 0);
		wr(venus_va + CPU_CS_BASE + CPU_CS_A2HSOFTINTCLR, 1);
		wr(venus_va + WRAPPER_BASE + WRAPPER_INTR_CLEAR, VENUS_INTR_ALL);
	}
	if (g_firmware_up) {
		uint64_t answer = ~0ull;
		int32_t result = venus_scm(g_scm_convention, SCM_PAS_SHUTDOWN, 1, venus.pas_id, 0, 0, &answer);

		g_firmware_up = false;
		snprintf(line, sizeof(line), "vayu-venus: quiesce: PAS shutdown: %d, answer %#llx", result, answer);
		loader_log_line(line);
	}
	if (g_smmu_match != ~0u) {
		smmu_down_quiesce();
		loader_log_line("vayu-venus: quiesce: the SMMU's match register and context put back");
	}
	if (videoc_va != 0) {
		if (g_rcg_set) {
			wr(videoc_va + VIDEOC_IRIS_CFG, g_rcg_old);
			wr(videoc_va + VIDEOC_IRIS_CMD, rd(videoc_va + VIDEOC_IRIS_CMD) | VIDEOC_CMD_UPDATE);
			g_rcg_set = false;
		}
		if (g_pll0_ours) {
			wr(videoc_va + VIDEOC_PLL0 + PLL_MODE,
			    rd(videoc_va + VIDEOC_PLL0 + PLL_MODE) & ~PLL_OUTCTRL);
			wr(videoc_va + VIDEOC_PLL0 + PLL_USER_CTL,
			    rd(videoc_va + VIDEOC_PLL0 + PLL_USER_CTL) & ~PLL_OUT_MASK);
			wr(videoc_va + VIDEOC_PLL0 + PLL_OPMODE, PLL_STANDBY);
			g_pll0_ours = false;
		}
		for (int i = (int)(sizeof(g_cc) / sizeof(g_cc[0])) - 1; i >= 0; i--) {
			if (*g_cc[i].ours) {
				wr(videoc_va + g_cc[i].cbcr, rd(videoc_va + g_cc[i].cbcr) & ~1u);
				*g_cc[i].ours = false;
			}
		}
		for (int i = (int)(sizeof(g_gdsc) / sizeof(g_gdsc[0])) - 1; i >= 0; i--) {
			if (g_wrapper_ours[i] && g_gdsc[i].ctrl != 0) {
				wr(venus_va + WRAPPER_BASE + g_gdsc[i].ctrl, g_wrapper_old[i]);
				g_wrapper_ours[i] = false;
			}
			if (g_gdsc_ours[i]) {
				if (g_gdsc[i].hw_trigger) {
					/* HW_CONTROL first, or the switch ignores SW_COLLAPSE (gdsc.c:222-241) */
					wr(videoc_va + g_gdsc[i].offset,
					    rd(videoc_va + g_gdsc[i].offset) & ~(1u << 1));
				}
				wr(videoc_va + g_gdsc[i].offset, rd(videoc_va + g_gdsc[i].offset) | 1u);
				g_gdsc_ours[i] = false;
			}
		}
		loader_log_line("vayu-venus: quiesce: PLL0, RCG, branches and GDSCs cut (no poll); the shared rails stay");
	}
	if (gcc_va != 0) {
		for (int i = (int)(sizeof(g_gcc) / sizeof(g_gcc[0])) - 1; i >= 0; i--) {
			if (*g_gcc[i].ours) {
				wr(gcc_va + g_gcc[i].cbcr, rd(gcc_va + g_gcc[i].cbcr) & ~1u);
				*g_gcc[i].ours = false;
			}
		}
	}
	bcm_down();                     /* the video NoC/DDR votes, if M1 took any */
	loader_log_line("vayu-venus: quiesce: done");
}

/* ---- the thread ----------------------------------------------------------------------------------- */

static void
venus_thread(void *arg __unused, wait_result_t wr __unused)
{
	/* The firmware may be in /fw of the root, which is not mounted yet when this begins. */
	for (unsigned waited = 0; waited < 100 && rootvnode == NULLVP; waited++) {
		IOSleep(200);
	}
	if (!vayu_soc_check("vayu-venus: 1")) {
		say("vayu-venus: 1: FAILED: not vayu's shared memory/command database; nothing is touched\n");
		thread_terminate(current_thread());
		return;
	}
	if (!step_m1()) {
		say("vayu-venus: 1: M1 failed; the power-down runs in reverse\n");
	}
	venus_down();
	say("vayu-venus: thread done\n");
	thread_terminate(current_thread());
}

void
vayu_venus_init(void)
{
	thread_t thread;

	if (g_inited) {
		return;
	}
	g_inited = true;
	if (!PE_parse_boot_argn("vayu_venus", &g_level, sizeof(g_level)) || g_level == 0) {
		return;
	}
	if (g_level > 1) {
		if (g_level > 2) {
			say("vayu-venus: vayu_venus=%u: the levels are 1 (M1) and 2 (M1+M2+M3); 2 is used\n",
			    g_level);
		}
		g_level = 2;
	}
	PE_parse_boot_argn("vayu_venus_wait", &g_wait_ms, sizeof(g_wait_ms));
	if (g_wait_ms < 100) {
		g_wait_ms = 100;
	}
	if (g_wait_ms > 30000) {
		g_wait_ms = 30000;
	}
	PE_parse_boot_argn("vayu_venus_secs", &g_secs, sizeof(g_secs));
	if (g_secs > 30) {
		g_secs = 30;
	}
	PE_parse_boot_argn("vayu_venus_rails", &g_rails, sizeof(g_rails));
	PE_parse_boot_argn("vayu_venus_level", &g_rail_level, sizeof(g_rail_level));
	if (g_rail_level == 0 || g_rail_level > 1024) {
		g_rail_level = 384;
	}
	PE_parse_boot_argn("vayu_venus_rate", &g_rate_mhz, sizeof(g_rate_mhz));
	if (g_rate_mhz != 200 && g_rate_mhz != 225 && g_rate_mhz != 300 && g_rate_mhz != 365 &&
	    g_rate_mhz != 432 && g_rate_mhz != 480) {
		say("vayu-venus: vayu_venus_rate=%u is not one of 200/225/300/365/432/480; 200 is used\n", g_rate_mhz);
		g_rate_mhz = 200;
	}
	PE_parse_boot_argn("vayu_venus_xo", &g_xo, sizeof(g_xo));
	PE_parse_boot_argn("vayu_venus_gdsc", &g_gdsc_mask, sizeof(g_gdsc_mask));
	g_gdsc_mask &= 7;
	if (g_gdsc_mask == 0) {
		g_gdsc_mask = 1;
	}
	if (g_level >= 2) {
		/* the encoder core's own GDSC; M1 alone leaves it as found */
		g_gdsc_mask |= 2;
	}
	PE_parse_boot_argn("vayu_venus_debug", &g_debug, sizeof(g_debug));
	PE_parse_boot_argn("vayu_venus_bcm", &g_bcm, sizeof(g_bcm));
	if (g_level >= 2) {
		uint32_t codec = 1, width = g_width, height = g_height, fps = g_fps;
		uint32_t bitrate = g_bitrate, frame_ms = g_frame_ms, dump = g_dump;

		PE_parse_boot_argn("vayu_venus_codec", &codec, sizeof(codec));
		PE_parse_boot_argn("vayu_venus_width", &width, sizeof(width));
		PE_parse_boot_argn("vayu_venus_height", &height, sizeof(height));
		PE_parse_boot_argn("vayu_venus_fps", &fps, sizeof(fps));
		PE_parse_boot_argn("vayu_venus_bitrate", &bitrate, sizeof(bitrate));
		PE_parse_boot_argn("vayu_venus_frame_ms", &frame_ms, sizeof(frame_ms));
		PE_parse_boot_argn("vayu_venus_dump", &dump, sizeof(dump));
		g_codec = codec == 0 ? VAYU_HFI_CODEC_H264 : VAYU_HFI_CODEC_HEVC;
		if (width < 128 || width > 1920 || (width & 1) != 0) {
			say("vayu-venus: vayu_venus_width=%u is not 128..1920 even; 1920 is used\n", width);
			width = 1920;
		}
		if (height < 128 || height > 1080 || (height & 1) != 0) {
			say("vayu-venus: vayu_venus_height=%u is not 128..1080 even; 1080 is used\n", height);
			height = 1080;
		}
		if (fps < 1 || fps > 60) {
			say("vayu-venus: vayu_venus_fps=%u is not 1..60; 30 is used\n", fps);
			fps = 30;
		}
		if (bitrate < 64000 || bitrate > 100000000) {
			say("vayu-venus: vayu_venus_bitrate=%u is not 64000..100000000; 8000000 is used\n", bitrate);
			bitrate = 8000000;
		}
		if (frame_ms < 100 || frame_ms > 30000) {
			frame_ms = 3000;
		}
		g_width = width;
		g_height = height;
		g_fps = fps;
		g_bitrate = bitrate;
		g_frame_ms = frame_ms;
		g_dump = dump != 0;
	}
	say("vayu-venus: vayu_venus=%u wait=%u secs=%u rails=%u level=%u rate=%u xo=%u gdsc=%#x debug=%#x bcm=%u\n",
	    g_level, g_wait_ms, g_secs, g_rails, g_rail_level, g_rate_mhz, g_xo, g_gdsc_mask, g_debug, g_bcm);
	if (g_level >= 2) {
		say("vayu-venus: M2/M3: %s %ux%u at %u fps, %u bit/s, %u ms for one frame, dump %s, session %#x\n",
		    g_codec == VAYU_HFI_CODEC_HEVC ? "HEVC" : "H.264", g_width, g_height, g_fps,
		    g_bitrate, g_frame_ms, g_dump ? "on" : "off", g_session);
	}
	if (kernel_thread_start(venus_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("vayu-venus: no thread; nothing is touched\n");
	}
}
