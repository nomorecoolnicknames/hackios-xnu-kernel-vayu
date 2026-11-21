/*
 * vayu IPA 4.1 / GSI 2.0 data transport, before MPSS and before WDS.
 *
 * Exact vayu-r-oss values (not the SM7325 driver): command EP5/ch4, LAN EP10/ch5,
 * WAN TX EP2/ch3 and WAN RX EP11/ch6. The GSI EE0 offset is 0x1c000 (GSI 2.0),
 * proven independently by vayu_qup_spi.c. Event rings are allocated dynamically.
 *
 * The four channels and the empty filter/route/header tables are real: every immediate
 * command must complete and the resulting SRAM headers are read back. data_ready is
 * never set by MMIO writes alone. A modem filter request not implemented yet clears it.
 * No default route, call, internet or configd success is claimed here.
 *
 * a10-gsi switches (e1-e4, 18.11: with vayu_ipa_data=1 the SoC stops right after "about to cmd",
 * the first GSI group of the command channel). All default 0, i.e. the old sequence:
 *   vayu_ipa_data_trace=1     (bit 0) from the cmd channel's setup until the tables are read back,
 *                             every single GSI/IPA register access is said first ("vayu-ipa-data: mmio
 *                             #N wr|rd <pa> (<name>) ...") and left as a persist breadcrumb, then a
 *                             pause of vayu_ipa_data_trace_ms (default 300) before the access itself;
 *                             a read's value follows ("mmio #N rd -> <value>"). Inside a poll loop only
 *                             the first read is said; the rest are breadcrumbs and one summary line.
 *                             Any nonzero value still gives the old per-packet lines (2: only those).
 *   vayu_gsi_mainline=1       channel_setup() as mainline v6.12 gsi.c does it for the v4.0 register set
 *                             (gsi_evt_ring_program, gsi_channel_program): EV_CH_E_CNTXT_8 MODT 32 /
 *                             MODC 1, CH_C_QOS WRR 15 for the command channel and 0 + USE_ESCAPE_BUF_ONLY
 *                             (bit 10) for the others, GPI scratch (max_outstanding_tre = tlv * 16,
 *                             outstanding_threshold = 2 * 16) with SCRATCH_3's low 16 bits kept, no
 *                             DOORBELL_1 and no ENDP_GSI_CFG1 read. ALLOCATE/START/... are polled
 *                             (interrupts stay off): SRC_CH/EV_IRQ bit or the CNTXT_0 state, and
 *                             each command says "vayu-ipa-data: mainline: ...".
 *   vayu_gsi_mainline=2       only: no DOORBELL_1 writes (event ring and channel), the rest as before.
 *   vayu_gsi_mainline=4       only: ENDP_GSI_CFG1 (0x1e42794 + 4 * ep) is not read.
 */
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <stdbool.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <sys/vayu_cell_net.h>
#include <sys/vayu_cell_qmap.h>
#include <sys/vayu_ipa.h>
#include <sys/vayu_ipa_data.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t thread);

#define IPA_PHYS                0x1e40000UL
#define IPA_SIZE                0x2000UL
#define IPA_DEBUG_PHYS          0x1e42000UL
#define IPA_DEBUG_SIZE          0x1000UL
#define IPA_SRAM_PHYS           0x1e50000UL   /* legacy v4.5 offset; real window comes from vayu_ipa_sram_base() */
#define IPA_SRAM_SIZE           0x2800UL
#define GSI_PHYS                0x1e04000UL
#define GSI_SIZE                0x28000UL
#define GSI_EE0                 0x1c000
#define ELEMENT_SIZE            16
#define IPA_POOL_SIZE           (1024 * 1024)
#define IPA_POOL_ALIGN          16384

#define CH_CNTXT(c, n)          (GSI_EE0 + 0x80 * (c) + 4 * (n))
#define CH_QOS(c)               (GSI_EE0 + 0x5c + 0x80 * (c))
#define CH_SCRATCH(c, n)        (GSI_EE0 + 0x60 + 0x80 * (c) + 4 * (n))
#define EV_CNTXT(e, n)          (GSI_EE0 + 0x1000 + 0x80 * (e) + 4 * (n))
#define CH_DOORBELL(c)          (GSI_EE0 + 0x2000 + 8 * (c))
#define EV_DOORBELL(e)          (GSI_EE0 + 0x2100 + 8 * (e))
#define GSI_STATUS              (GSI_EE0 + 0x3000)
#define GSI_CH_CMD              (GSI_EE0 + 0x3008)
#define GSI_EV_CMD              (GSI_EE0 + 0x3010)
#define GSI_HW_PARAM_2          (GSI_EE0 + 0x3040)
#define GSI_SW_VERSION          (GSI_EE0 + 0x3044)
#define CNTXT_TYPE_IRQ          (GSI_EE0 + 0x3080)
#define CNTXT_TYPE_IRQ_MSK      (GSI_EE0 + 0x3088)
#define CNTXT_SRC_CH_IRQ        (GSI_EE0 + 0x3090)
#define CNTXT_SRC_EV_IRQ        (GSI_EE0 + 0x3094)
#define CNTXT_SRC_CH_IRQ_MSK    (GSI_EE0 + 0x3098)
#define CNTXT_SRC_EV_IRQ_MSK    (GSI_EE0 + 0x309c)
#define CNTXT_SRC_CH_IRQ_CLR    (GSI_EE0 + 0x30a0)
#define CNTXT_SRC_EV_IRQ_CLR    (GSI_EE0 + 0x30a4)
#define CNTXT_SRC_IEOB_IRQ_MSK  (GSI_EE0 + 0x30b8)
#define CNTXT_SRC_IEOB_IRQ_CLR  (GSI_EE0 + 0x30c0)
#define CNTXT_GLOB_IRQ_STTS     (GSI_EE0 + 0x3100)
#define CNTXT_GLOB_IRQ_EN       (GSI_EE0 + 0x3108)
#define CNTXT_GLOB_IRQ_CLR      (GSI_EE0 + 0x3110)
#define CNTXT_GSI_IRQ_STTS      (GSI_EE0 + 0x3118)
#define CNTXT_GSI_IRQ_EN        (GSI_EE0 + 0x3120)
#define CNTXT_GSI_IRQ_CLR       (GSI_EE0 + 0x3128)
#define CNTXT_INTSET            (GSI_EE0 + 0x3180)
#define CNTXT_MSI_LSB           (GSI_EE0 + 0x3188)
#define CNTXT_MSI_MSB           (GSI_EE0 + 0x318c)
#define GSI_ERROR_LOG           (GSI_EE0 + 0x3200)
#define GSI_ERROR_LOG_CLR       (GSI_EE0 + 0x3210)
#define CNTXT_SCRATCH_0         (GSI_EE0 + 0x3400)
#define CNTXT_SCRATCH_1         (GSI_EE0 + 0x3404)

#define GSI_ALLOCATE            0
#define GSI_START               1
#define GSI_STOP                2
#define GSI_RESET               9
#define GSI_DEALLOCATE          10
#define GSI_TYPE_GPI            2
#define GSI_EVT_TYPE_GPI        2
#define GSI_EVT_RING_INT_MODT   32              /* mainline gsi.c: 1 ms of the 32 kHz clock */
#define GSI_ML_SEQUENCE         1u              /* vayu_gsi_mainline bits */
#define GSI_ML_NO_DB1           2u
#define GSI_ML_NO_CFG1          4u

#define ENDP_CTRL(ep)           (0x800 + 0x70 * (ep))
#define ENDP_CFG(ep)            (0x808 + 0x70 * (ep))
#define ENDP_NAT(ep)            (0x80c + 0x70 * (ep))
#define ENDP_HDR(ep)            (0x810 + 0x70 * (ep))
#define ENDP_HDR_EXT(ep)        (0x814 + 0x70 * (ep))
#define ENDP_METADATA_MASK(ep)  (0x818 + 0x70 * (ep))
#define ENDP_MODE(ep)           (0x820 + 0x70 * (ep))
#define ENDP_AGGR(ep)           (0x824 + 0x70 * (ep))
#define ENDP_HOLB(ep)           (0x82c + 0x70 * (ep))
#define ENDP_DEAGGR(ep)         (0x834 + 0x70 * (ep))
#define ENDP_RSRC_GRP(ep)       (0x838 + 0x70 * (ep))
#define ENDP_SEQ(ep)            (0x83c + 0x70 * (ep))
#define ENDP_STATUS(ep)         (0x840 + 0x70 * (ep))
#define ENDP_GSI_CFG1(ep)       (0x794 + 4 * (ep))
#define IPA_ENABLED_PIPES       0x38
#define IPA_LOCAL_PROC_BASE     0x1e8
#define IPA_HASH_FLUSH          0x14c

#define IPA_CMD_V4_FILTER       3
#define IPA_CMD_V6_FILTER       4
#define IPA_CMD_V4_ROUTE        7
#define IPA_CMD_V6_ROUTE        8
#define IPA_CMD_HDR_LOCAL       9
#define IPA_CMD_DMA_SHARED      19
#define TRE_CHAIN               (1u << 0)
#define TRE_IEOT                (1u << 9)
#define TRE_BEI                 (1u << 10)
#define TRE_TYPE_XFER           (2u << 16)
#define TRE_TYPE_COMMAND        (3u << 16)
#define GSI_EVT_SUCCESS         1
#define GSI_EVT_EOT             2
#define GSI_EVT_EOB             4

static int say(const char *format, ...) __printflike(1, 2);
static int
say(const char *format, ...)
{
	char line[320];
	va_list list;
	int length;
	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) loader_log_putc(*c);
	printf("%s", line);
	return length;
}

static vm_offset_t ipa_regs, ipa_debug, gsi_regs;
static LCK_GRP_DECLARE(ipa_data_lock_group, "vayu-ipa-data");
static LCK_MTX_DECLARE(ipa_data_lock, &ipa_data_lock_group);

/*
 * vayu_ipa_data_trace=1: the per-access trace. e1-e4 stopped somewhere in the cmd group with the
 * line before it already out; a line and a breadcrumb before each access, then a pause long enough
 * for the line to leave, make the last line in the live log the access that never finished.
 */
static bool mmio_trace, mmio_quiet;
static uint32_t mmio_trace_ms = 300, mmio_seq, mmio_quiet_reads, mmio_quiet_pa, mmio_quiet_value;
static char mmio_tag[32];
static uint32_t gsi_mainline;

static uint32_t
mmio_pa(char block, uint32_t off)
{
	return (uint32_t)(block == 'G' ? GSI_PHYS : block == 'I' ? IPA_PHYS : IPA_DEBUG_PHYS) + off;
}

static void
mmio_name(char block, uint32_t off, char *name, size_t size)
{
	static const struct { uint32_t off; const char *name; } gsi_fixed[] = {
		{ GSI_STATUS, "GSI_STATUS" }, { GSI_CH_CMD, "CH_CMD" }, { GSI_EV_CMD, "EV_CH_CMD" },
		{ GSI_HW_PARAM_2, "HW_PARAM_2" }, { GSI_SW_VERSION, "SW_VERSION" },
		{ CNTXT_TYPE_IRQ, "CNTXT_TYPE_IRQ" }, { CNTXT_TYPE_IRQ_MSK, "CNTXT_TYPE_IRQ_MSK" },
		{ CNTXT_SRC_CH_IRQ, "CNTXT_SRC_CH_IRQ" }, { CNTXT_SRC_EV_IRQ, "CNTXT_SRC_EV_IRQ" },
		{ CNTXT_SRC_CH_IRQ_MSK, "CNTXT_SRC_CH_IRQ_MSK" }, { CNTXT_SRC_EV_IRQ_MSK, "CNTXT_SRC_EV_IRQ_MSK" },
		{ CNTXT_SRC_CH_IRQ_CLR, "CNTXT_SRC_CH_IRQ_CLR" }, { CNTXT_SRC_EV_IRQ_CLR, "CNTXT_SRC_EV_IRQ_CLR" },
		{ CNTXT_SRC_IEOB_IRQ_MSK, "CNTXT_SRC_IEOB_MSK" }, { CNTXT_SRC_IEOB_IRQ_CLR, "CNTXT_SRC_IEOB_CLR" },
		{ CNTXT_GLOB_IRQ_STTS, "CNTXT_GLOB_IRQ_STTS" }, { CNTXT_GLOB_IRQ_EN, "CNTXT_GLOB_IRQ_EN" },
		{ CNTXT_GLOB_IRQ_CLR, "CNTXT_GLOB_IRQ_CLR" }, { CNTXT_GSI_IRQ_STTS, "CNTXT_GSI_IRQ_STTS" },
		{ CNTXT_GSI_IRQ_EN, "CNTXT_GSI_IRQ_EN" }, { CNTXT_GSI_IRQ_CLR, "CNTXT_GSI_IRQ_CLR" },
		{ CNTXT_INTSET, "CNTXT_INTSET" }, { CNTXT_MSI_LSB, "CNTXT_MSI_LSB" }, { CNTXT_MSI_MSB, "CNTXT_MSI_MSB" },
		{ GSI_ERROR_LOG, "ERROR_LOG" }, { GSI_ERROR_LOG_CLR, "ERROR_LOG_CLR" },
		{ CNTXT_SCRATCH_0, "CNTXT_SCRATCH_0" }, { CNTXT_SCRATCH_1, "CNTXT_SCRATCH_1" },
	};
	static const char *const endp[0x70 / 4] = {
		[0x00 / 4] = "CTRL", [0x08 / 4] = "CFG", [0x0c / 4] = "NAT", [0x10 / 4] = "HDR",
		[0x14 / 4] = "HDR_EXT", [0x18 / 4] = "HDR_METADATA_MASK", [0x20 / 4] = "MODE",
		[0x24 / 4] = "AGGR", [0x2c / 4] = "HOL_BLOCK_EN", [0x34 / 4] = "DEAGGR",
		[0x38 / 4] = "RSRC_GRP", [0x3c / 4] = "SEQ", [0x40 / 4] = "STATUS",
	};

	if (block == 'D') {
		if (off >= 0x794 && off < 0x794 + 4 * 32 && (off & 3) == 0) {
			snprintf(name, size, "EP%u_GSI_CFG1", (off - 0x794) / 4);
		} else {
			snprintf(name, size, "IPA_DEBUG+%#x", off);
		}
		return;
	}
	if (block == 'I') {
		uint32_t r = (off - 0x800) % 0x70;

		if (off >= 0x800 && off < 0x800 + 0x70 * 32 && (r & 3) == 0 && endp[r / 4] != NULL) {
			snprintf(name, size, "EP%u_%s", (off - 0x800) / 0x70, endp[r / 4]);
		} else {
			if (off == IPA_ENABLED_PIPES) strlcpy(name, "ENABLED_PIPES", size);
			else if (off == IPA_LOCAL_PROC_BASE) strlcpy(name, "LOCAL_PROC_BASE", size);
			else if (off == IPA_HASH_FLUSH) strlcpy(name, "HASH_FLUSH", size);
			else snprintf(name, size, "IPA+%#x", off);
		}
		return;
	}
	for (unsigned i = 0; i < sizeof(gsi_fixed) / sizeof(gsi_fixed[0]); i++) {
		if (gsi_fixed[i].off == off) {
			strlcpy(name, gsi_fixed[i].name, size);
			return;
		}
	}
	if (off >= GSI_EE0 && off < GSI_EE0 + 0x1000) {
		uint32_t c = (off - GSI_EE0) / 0x80, r = (off - GSI_EE0) % 0x80;

		if (r < 0x10) snprintf(name, size, "CH%u_CNTXT_%u", c, r / 4);
		else if (r == 0x5c) snprintf(name, size, "CH%u_QOS", c);
		else if (r >= 0x60 && r < 0x70) snprintf(name, size, "CH%u_SCRATCH_%u", c, (r - 0x60) / 4);
		else snprintf(name, size, "CH%u+%#x", c, r);
	} else if (off >= GSI_EE0 + 0x1000 && off < GSI_EE0 + 0x2000) {
		uint32_t e = (off - GSI_EE0 - 0x1000) / 0x80, r = (off - GSI_EE0 - 0x1000) % 0x80;

		if (r < 0x38) snprintf(name, size, "EV%u_CNTXT_%u", e, r / 4);
		else snprintf(name, size, "EV%u+%#x", e, r);
	} else if (off >= GSI_EE0 + 0x2000 && off < GSI_EE0 + 0x2200) {
		uint32_t at = off - GSI_EE0 - 0x2000;

		snprintf(name, size, "%s%u_DOORBELL_%u", at < 0x100 ? "CH" : "EV", (at & 0xff) / 8, (at & 7) / 4);
	} else {
		snprintf(name, size, "GSI+%#x", off);
	}
}

static void
mmio_before(char block, uint32_t off, bool write, uint32_t value)
{
	char name[32];
	uint32_t pa = mmio_pa(block, off);

	mmio_seq++;
	if (!write && mmio_quiet) {
		mmio_quiet_reads++;
		mmio_quiet_pa = pa;
		snprintf(mmio_tag, sizeof(mmio_tag), "d#%u rd poll", mmio_seq);
		vayu_ipa_data_mark(mmio_tag, pa, false, false);
		return;
	}
	mmio_name(block, off, name, sizeof(name));
	if (write) {
		say("vayu-ipa-data: mmio #%u wr %#x (%s) <- %#x, cpu %u\n", mmio_seq, pa, name, value,
		    (unsigned)cpu_number());
	} else {
		say("vayu-ipa-data: mmio #%u rd %#x (%s), cpu %u\n", mmio_seq, pa, name, (unsigned)cpu_number());
	}
	snprintf(mmio_tag, sizeof(mmio_tag), "d#%u %s %s", mmio_seq, write ? "wr" : "rd", name);
	vayu_ipa_data_mark(mmio_tag, pa, write, false);
	if (mmio_trace_ms != 0) delay_for_interval(mmio_trace_ms, NSEC_PER_MSEC);
}

static void
mmio_after(char block, uint32_t off, bool write, uint32_t value)
{
	vayu_ipa_data_mark(mmio_tag, mmio_pa(block, off), write, true);
	if (write) return;
	if (mmio_quiet) {
		mmio_quiet_value = value;
	} else {
		say("vayu-ipa-data: mmio #%u rd -> %#x\n", mmio_seq, value);
	}
}

/* Poll loops: after the first round only breadcrumbs, then one line for the rest. */
static inline void
poll_quiet(void)
{
	mmio_quiet = mmio_trace;
}
static void
poll_end(void)
{
	if (mmio_quiet && mmio_quiet_reads != 0) {
		say("vayu-ipa-data: mmio poll: %u more reads, the last %#x -> %#x (up to #%u)\n", mmio_quiet_reads,
		    mmio_quiet_pa, mmio_quiet_value, mmio_seq);
	}
	mmio_quiet = false;
	mmio_quiet_reads = 0;
}

static inline uint32_t
mmio_rd(char block, vm_offset_t base, uint32_t off)
{
	uint32_t value;

	if (mmio_trace) mmio_before(block, off, false, 0);
	value = *(volatile uint32_t *)(base + off);
	if (mmio_trace) mmio_after(block, off, false, value);
	return value;
}
static inline void
mmio_wr(char block, vm_offset_t base, uint32_t off, uint32_t value)
{
	if (mmio_trace) mmio_before(block, off, true, value);
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb sy" ::: "memory");
	if (mmio_trace) mmio_after(block, off, true, value);
}

static inline uint32_t
ipa_rd(uint32_t off) { return mmio_rd('I', ipa_regs, off); }
static inline void
ipa_wr(uint32_t off, uint32_t value) { mmio_wr('I', ipa_regs, off, value); }
static inline uint32_t
debug_rd(uint32_t off) { return mmio_rd('D', ipa_debug, off); }
static inline uint32_t
gsi_rd(uint32_t off) { return mmio_rd('G', gsi_regs, off); }
static inline void
gsi_wr(uint32_t off, uint32_t value) { mmio_wr('G', gsi_regs, off, value); }
static inline uint64_t
sram_rd64(vm_offset_t sram, uint32_t off) { return *(volatile uint64_t *)(sram + off); }

struct dma {
	vm_offset_t virt;
	uint64_t phys;
};

static uint8_t ipa_pool[IPA_POOL_SIZE] __attribute__((aligned(IPA_POOL_ALIGN)));
static uint64_t pool_phys;
static size_t pool_used;

static bool
pool_init(void)
{
	pool_phys = kvtophys((vm_offset_t)ipa_pool);
	if (pool_phys < 0x80000000ULL || pool_phys + sizeof(ipa_pool) - 1 > 0xffffffffULL ||
	    (pool_phys & 0xfff)) {
		say("vayu-ipa-data: pool phys %#llx is outside the IPA SMMU DDR identity window\n",
		    (unsigned long long)pool_phys);
		return false;
	}
	for (size_t at = 0; at < sizeof(ipa_pool); at += PAGE_SIZE) {
		if (kvtophys((vm_offset_t)ipa_pool + at) != pool_phys + at) {
			say("vayu-ipa-data: pool is not contiguous at %#zx\n", at);
			return false;
		}
	}
	pool_used = 0;                       /* a failed/stopped prepare may be retried */
	memset(ipa_pool, 0, sizeof(ipa_pool));
	say("vayu-ipa-data: DMA pool %#llx..%#llx, physically contiguous, IOVA=PA\n",
	    (unsigned long long)pool_phys, (unsigned long long)(pool_phys + sizeof(ipa_pool) - 1));
	return true;
}

static bool
pool_take(struct dma *d, size_t size, size_t align)
{
	size_t at = (pool_used + align - 1) & ~(align - 1);

	if (align == 0 || (align & (align - 1)) || at > sizeof(ipa_pool) || size > sizeof(ipa_pool) - at) {
		say("vayu-ipa-data: pool exhausted: used %#zx need %#zx align %#zx\n", pool_used, size, align);
		return false;
	}
	d->virt = (vm_offset_t)ipa_pool + at;
	d->phys = pool_phys + at;
	pool_used = at + size;
	memset((void *)d->virt, 0, size);
	return true;
}

static void
dma_sync(vm_offset_t virt, size_t size)
{
	vm_offset_t from = virt & ~63ul;

	FlushPoC_DcacheRegion(from, ((virt + size + 63) & ~63ul) - from);
	__asm__ volatile("dsb sy" ::: "memory");
}

struct tre {
	uint64_t addr;
	uint16_t len_opcode;
	uint16_t reserved;
	uint32_t flags;
};
struct event {
	uint64_t xfer;
	uint16_t len;
	uint8_t reserved1;
	uint8_t code;
	uint16_t reserved2;
	uint8_t type;
	uint8_t chid;
};
static_assert(sizeof(struct tre) == 16, "GSI TRE is 16 bytes");
static_assert(sizeof(struct event) == 16, "GSI event is 16 bytes");

struct channel {
	const char *name;
	uint8_t id, endpoint;
	uint32_t event_id;
	bool to_ipa, command, qmap, aggregate, close_eof;
	uint32_t slots, buffer_size, low_weight;
	uint32_t tlv;                            /* msm-4.14 ipa_utils.c IPA_4_1 ipa_if_tlv */
	struct dma tre_ring, event_ring, data;
	uint32_t next, done, events;
	bool event_allocated, allocated, started;
	uint64_t packets, bytes, dropped, errors;
};

static struct channel channels[] = {
	{ .name = "cmd", .id = 4, .endpoint = 5, .event_id = ~0u, .to_ipa = true,
	  .command = true, .slots = 16, .buffer_size = 256, .low_weight = 15, .tlv = 20 },
	{ .name = "lan-rx", .id = 5, .endpoint = 10, .event_id = ~0u,
	  .aggregate = true, .slots = 8, .buffer_size = 8192, .low_weight = 1, .tlv = 9 },
	{ .name = "wan-tx", .id = 3, .endpoint = 2, .event_id = ~0u, .to_ipa = true,
	  .qmap = true, .slots = 64, .buffer_size = 2048, .low_weight = 1, .tlv = 16 },
	{ .name = "wan-rx", .id = 6, .endpoint = 11, .event_id = ~0u,
	  .qmap = true, .aggregate = true, .close_eof = true, .slots = 16,
	  .buffer_size = 16384, .low_weight = 1, .tlv = 9 },
};
#define NCHANNELS (sizeof(channels) / sizeof(channels[0]))
#define COMMAND (&channels[0])
#define LAN_RX (&channels[1])
#define WAN_TX (&channels[2])
#define WAN_RX (&channels[3])
static uint32_t max_event_rings;
static bool prepared, ready, filter_failed, stopping, thread_running;
static uint32_t trace;
static bool first_packet_seen;
/* QMAP boundary counters: the demux is mux 1 only; command frames are not acted on because WDA
 * asked for no checksum offload (vayu_cell_data.c), so there is no MAP command we must answer. */
static uint32_t qmap_commands, qmap_unknown_mux;
static bool qmap_rx_logged;

/* vayu-data: S10: the first packet either way, once. The counters in vayu_cell_net_get_status()
 * carry the rest. */
static void
first_packet(const char *way, const char *channel, uint32_t bytes)
{
	if (first_packet_seen) return;
	first_packet_seen = true;
	say("vayu-data: S10 FIRST PACKET: OK %s via %s, %u bytes\n", way, channel, bytes);
}

static inline vm_offset_t
slot_virt(const struct channel *ch, uint32_t slot)
{
	return ch->data.virt + (vm_offset_t)slot * ch->buffer_size;
}
static inline uint64_t
slot_phys(const struct channel *ch, uint32_t slot)
{
	return ch->data.phys + (uint64_t)slot * ch->buffer_size;
}
static inline struct tre *
tre_at(struct channel *ch, uint32_t index)
{
	return (struct tre *)(ch->tre_ring.virt + (index % ch->slots) * ELEMENT_SIZE);
}

static void
gsi_error(const char *what)
{
	uint32_t log = gsi_rd(GSI_ERROR_LOG);

	say("vayu-ipa-data: %s: GSI status %#x error %#x (code %u type %u EE %u index %u), global %#x general %#x\n",
	    what, gsi_rd(GSI_STATUS), log, (log >> 12) & 0xf, (log >> 24) & 0xf,
	    log >> 28, (log >> 19) & 0x1f, gsi_rd(CNTXT_GLOB_IRQ_STTS), gsi_rd(CNTXT_GSI_IRQ_STTS));
}

static uint32_t
channel_state(const struct channel *ch, bool event)
{
	return (gsi_rd(event ? EV_CNTXT(ch->event_id, 0) : CH_CNTXT(ch->id, 0)) >> 20) & 0xf;
}

static bool
gsi_command(struct channel *ch, bool event, uint32_t opcode, uint32_t wanted)
{
	uint32_t number = event ? ch->event_id : ch->id, state = 0, source = 0, us;
	uint32_t source_reg = event ? CNTXT_SRC_EV_IRQ : CNTXT_SRC_CH_IRQ;
	bool mainline = (gsi_mainline & GSI_ML_SEQUENCE) != 0;

	/* vayu_gsi_mainline=1: gsi_irq_{ch,ev}_ctrl_enable() clears every stale completion first; the
	 * completion mainline waits for is this context's bit in CNTXT_SRC_{CH,EV_CH}_IRQ (gsi_isr_*_ctrl),
	 * polled here because the GSI interrupt stays masked. */
	if (mainline) gsi_wr(event ? CNTXT_SRC_EV_IRQ_CLR : CNTXT_SRC_CH_IRQ_CLR, ~0u);
	gsi_wr(event ? GSI_EV_CMD : GSI_CH_CMD, number | opcode << 24);
	for (us = 0; us < 250000; us += 100) {
		if (mainline) {
			source = gsi_rd(source_reg);
			if (source & 1u << number) break;
		}
		state = channel_state(ch, event);
		if (state == wanted) break;
		poll_quiet();
		delay_for_interval(100, NSEC_PER_USEC);
	}
	poll_end();
	if (mainline) {
		uint32_t type = gsi_rd(CNTXT_TYPE_IRQ);

		if (!(source & 1u << number)) source = gsi_rd(source_reg);
		state = channel_state(ch, event);
		say("vayu-ipa-data: mainline: %s command %u on %s %u: state %u (wanted %u) after %u us, "
		    "TYPE_IRQ %#x, SRC_%s_IRQ %#x\n", ch->name, opcode, event ? "event" : "channel", number, state,
		    wanted, us, type, event ? "EV" : "CH", source);
	}
	gsi_wr(event ? CNTXT_SRC_EV_IRQ_CLR : CNTXT_SRC_CH_IRQ_CLR, 1u << number);
	if (state != wanted) {
		say("vayu-ipa-data: %s command %#x on %s %u: state %u, expected %u\n",
		    ch->name, opcode, event ? "event" : "channel", number, state, wanted);
		gsi_error(ch->name);
		return false;
	}
	return true;
}

static void
gsi_interrupts_off(void)
{
	gsi_wr(CNTXT_INTSET, 1);               /* IRQ mode, but every source is polled/masked */
	gsi_wr(CNTXT_MSI_LSB, 0);
	gsi_wr(CNTXT_MSI_MSB, 0);
	gsi_wr(GSI_ERROR_LOG, 0);
	gsi_wr(CNTXT_TYPE_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_CH_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_EV_IRQ_MSK, 0);
	gsi_wr(CNTXT_SRC_IEOB_IRQ_MSK, 0);
	gsi_wr(CNTXT_GLOB_IRQ_EN, 0);
	gsi_wr(CNTXT_GSI_IRQ_EN, 0);
	gsi_wr(CNTXT_SRC_CH_IRQ_CLR, ~0u);
	gsi_wr(CNTXT_SRC_EV_IRQ_CLR, ~0u);
	gsi_wr(CNTXT_SRC_IEOB_IRQ_CLR, ~0u);
	gsi_wr(CNTXT_GLOB_IRQ_CLR, ~0u);
	gsi_wr(CNTXT_GSI_IRQ_CLR, ~0u);
	gsi_wr(GSI_ERROR_LOG_CLR, ~0u);
	gsi_wr(CNTXT_SCRATCH_0, 0);
	gsi_wr(CNTXT_SCRATCH_1, 0);
}

static bool
event_claim(struct channel *ch)
{
	for (uint32_t e = 0; e < max_event_rings; e++) {
		bool used = false;
		for (unsigned i = 0; i < NCHANNELS; i++) {
			if (channels[i].event_allocated && channels[i].event_id == e) used = true;
		}
		if (!used && ((gsi_rd(EV_CNTXT(e, 0)) >> 20) & 0xf) == 0) {
			ch->event_id = e;
			return true;
		}
	}
	say("vayu-ipa-data: %s: no free event ring\n", ch->name);
	return false;
}

static bool
endpoint_check(const struct channel *ch)
{
	uint32_t expected = 1u << 16 | (uint32_t)ch->id << 8, cfg = 0;

	/* TZ/FW owns this binding. It may still be finishing ipa_fws startup; never
	 * write the register, but allow a bounded 100 ms for it to appear. */
	for (unsigned ms = 0; ms < 100; ms++) {
		cfg = debug_rd(ENDP_GSI_CFG1(ch->endpoint));
		if ((cfg & 0x11f0f) == expected) {
			poll_end();
			return true;
		}
		poll_quiet();
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	poll_end();
	say("vayu-ipa-data: EP%u GSI_CFG1 %#x, expected %#x after 100 ms (TZ owns binding); refusing\n",
	    ch->endpoint, cfg, expected);
	return false;
}

static void
endpoint_program(const struct channel *ch)
{
	uint32_t ep = ch->endpoint, value;

	ipa_wr(ENDP_CTRL(ep), 0);
	ipa_wr(ENDP_CFG(ep), 0);                    /* no checksum offload */
	ipa_wr(ENDP_NAT(ep), 0);
	value = 0;
	if (ch->qmap) {
		value = 4 | 1u << 6 | 2u << 20;
		if (!ch->to_ipa) {
			/* RX: metadata in header byte 1 and packet size at bytes 2..3 is valid. */
			value |= 1u << 7 | 1u << 19;
		}
	}
	/* LAN exceptions have no header at this stage: IPA_ROUTE does not retain one. */
	ipa_wr(ENDP_HDR(ep), value);
	value = 0;
	if (ch->qmap) {
		value = 1 | 1u << 1 | 1u << 3;        /* big endian, pad valid, length includes pad */
		if (ch->to_ipa) value |= 2u << 10;    /* 4-byte alignment */
	} else if (!ch->to_ipa) {
		value = 1 | 1u << 1 | 2u << 10;       /* LAN header padding */
	}
	ipa_wr(ENDP_HDR_EXT(ep), value);
	ipa_wr(ENDP_METADATA_MASK(ep), !ch->to_ipa && ch->qmap ? 0xff000000 : 0);
	if (ch->to_ipa) ipa_wr(ENDP_MODE(ep), ch->command ? 3 | LAN_RX->endpoint << 4 : 0);
	value = 0;
	if (!ch->to_ipa && ch->aggregate) {
		uint32_t kb = ch->qmap ? 14 : 6;       /* WDA 0x3800 = 14 KiB; stock LAN = 6 */
		uint32_t packets = ch->qmap ? 16 : 0;
		value = 1 | 4u << 2 | kb << 5 | 1u << 10 | packets << 15 |
		    (ch->close_eof ? 1u << 21 : 0);  /* generic, 0.5 ms tick */
	} else if (ch->to_ipa && ch->qmap) {
		value = 2 | 6u << 2;                  /* deaggregate QCMAP */
	}
	ipa_wr(ENDP_AGGR(ep), value);
	if (ch->to_ipa) ipa_wr(ENDP_DEAGGR(ep), 0);
	else ipa_wr(ENDP_HOLB(ep), 0);
	ipa_wr(ENDP_RSRC_GRP(ep), 1);              /* IPA_v4_0_GROUP_UL_DL */
	if (ch->to_ipa) ipa_wr(ENDP_SEQ(ep), ch->command ? 0 : 0x804);
	ipa_wr(ENDP_STATUS(ep), 0);
}

static bool
channel_setup(struct channel *ch)
{
	uint32_t c = ch->id;
	bool mainline = (gsi_mainline & GSI_ML_SEQUENCE) != 0;
	bool doorbell_1 = (gsi_mainline & (GSI_ML_SEQUENCE | GSI_ML_NO_DB1)) == 0;
	bool gsi_cfg1 = (gsi_mainline & (GSI_ML_SEQUENCE | GSI_ML_NO_CFG1)) == 0;

	if (!event_claim(ch)) return false;
	if (!gsi_cfg1) {
		say("vayu-ipa-data: %s: EP%u GSI_CFG1 not read (vayu_gsi_mainline=%u; mainline never reads it)\n",
		    ch->name, ch->endpoint, gsi_mainline);
	} else if (!endpoint_check(ch)) {
		return false;
	}
	if (((gsi_rd(CH_CNTXT(c, 0)) >> 20) & 0xf) != 0) {
		say("vayu-ipa-data: %s channel %u is already in use\n", ch->name, c);
		return false;
	}
	if (!gsi_command(ch, true, GSI_ALLOCATE, 1)) return false;
	ch->event_allocated = true;
	uint32_t e = ch->event_id;
	gsi_wr(EV_CNTXT(e, 0), GSI_EVT_TYPE_GPI | 1u << 16 | ELEMENT_SIZE << 24);
	gsi_wr(EV_CNTXT(e, 1), ch->slots * ELEMENT_SIZE);
	gsi_wr(EV_CNTXT(e, 2), (uint32_t)ch->event_ring.phys);
	gsi_wr(EV_CNTXT(e, 3), (uint32_t)(ch->event_ring.phys >> 32));
	/* mainline gsi_evt_ring_program: moderation MODT 1 ms, MODC 1, no counter; 9..13 (MSI data and
	 * address, RP update address) zero, as here. */
	gsi_wr(EV_CNTXT(e, 8), mainline ? GSI_EVT_RING_INT_MODT | 1u << 16 : 0);
	for (unsigned n = 9; n <= 13; n++) gsi_wr(EV_CNTXT(e, n), 0);
	if (doorbell_1) gsi_wr(EV_DOORBELL(e) + 4, (uint32_t)(ch->event_ring.phys >> 32));
	gsi_wr(EV_DOORBELL(e), (uint32_t)(ch->event_ring.phys + (ch->slots - 1) * ELEMENT_SIZE));

	if (!gsi_command(ch, false, GSI_ALLOCATE, 1)) return false;
	ch->allocated = true;
	gsi_wr(CH_CNTXT(c, 0), GSI_TYPE_GPI | (ch->to_ipa ? 1u << 3 : 0) |
	    e << 14 | ELEMENT_SIZE << 24);
	gsi_wr(CH_CNTXT(c, 1), ch->slots * ELEMENT_SIZE);
	gsi_wr(CH_CNTXT(c, 2), (uint32_t)ch->tre_ring.phys);
	gsi_wr(CH_CNTXT(c, 3), (uint32_t)(ch->tre_ring.phys >> 32));
	if (mainline) {
		/* gsi_channel_program, v4.0..v4.4: the command channel the highest WRR weight and the
		 * prefetch buffers; every other channel weight 0 and the escape buffer only (bit 10). */
		gsi_wr(CH_QOS(c), ch->command ? 15 : 1u << 10);
		/* GPI scratch: word 3 bits 16..31 max_outstanding_tre, word 4 bits 16..31
		 * outstanding_threshold; SCRATCH_3's bits 0..15 belong to the firmware and are kept. */
		gsi_wr(CH_SCRATCH(c, 0), 0);
		gsi_wr(CH_SCRATCH(c, 1), 0);
		gsi_wr(CH_SCRATCH(c, 2), (ch->tlv * ELEMENT_SIZE) << 16);
		uint32_t keep = gsi_rd(CH_SCRATCH(c, 3)) & 0xffff;
		gsi_wr(CH_SCRATCH(c, 3), (2u * ELEMENT_SIZE) << 16 | keep);
	} else {
		gsi_wr(CH_QOS(c), ch->low_weight);     /* direct mode, one prefetch, no escape-only */
		for (unsigned n = 0; n < 4; n++) gsi_wr(CH_SCRATCH(c, n), 0); /* v4.0+: no MCS smart prefetch */
	}
	if (doorbell_1) gsi_wr(CH_DOORBELL(c) + 4, (uint32_t)(ch->tre_ring.phys >> 32));
	endpoint_program(ch);
	return true;
}

static void
tre_fill(struct channel *ch, uint32_t index, uint64_t addr, uint16_t len_opcode, uint32_t flags)
{
	struct tre *t = tre_at(ch, index);
	t->addr = addr;
	t->len_opcode = len_opcode;
	t->reserved = 0;
	t->flags = flags;
	dma_sync((vm_offset_t)t, sizeof(*t));
}

static void
channel_doorbell(struct channel *ch)
{
	gsi_wr(CH_DOORBELL(ch->id), (uint32_t)(ch->tre_ring.phys + (ch->next % ch->slots) * ELEMENT_SIZE));
}

static void
receive_post(struct channel *ch)
{
	tre_fill(ch, ch->next, slot_phys(ch, ch->next % ch->slots),
	    (uint16_t)ch->buffer_size, TRE_TYPE_XFER | TRE_IEOT | TRE_BEI);
	ch->next++;
}

static bool
channel_start(struct channel *ch)
{
	if (!gsi_command(ch, false, GSI_START, 2)) return false;
	ch->started = true;
	if (!ch->to_ipa) {
		for (uint32_t slot = 0; slot + 1 < ch->slots; slot++) receive_post(ch);
		channel_doorbell(ch);                 /* slots-1: write==read means empty */
	}
	say("vayu-ipa-data: %s started: EP%u ch%u event %u, %u slots, buffers %u bytes\n",
	    ch->name, ch->endpoint, ch->id, ch->event_id, ch->slots, ch->buffer_size);
	return true;
}

static unsigned
channel_events(struct channel *ch, void (*each)(struct channel *, const struct event *, void *), void *context)
{
	uint32_t base = (uint32_t)ch->event_ring.phys, wp = gsi_rd(EV_CNTXT(ch->event_id, 4));
	unsigned count = 0;

	if (wp < base || wp >= base + ch->slots * ELEMENT_SIZE || ((wp - base) & 0xf)) {
		if (ch->errors++ < 4) say("vayu-ipa-data: %s event WP %#x outside ring %#x\n", ch->name, wp, base);
		return 0;
	}
	uint32_t target = (wp - base) / ELEMENT_SIZE;
	while (ch->events % ch->slots != target) {
		struct event *e = (struct event *)(ch->event_ring.virt + (ch->events % ch->slots) * ELEMENT_SIZE);
		dma_sync((vm_offset_t)e, sizeof(*e));
		each(ch, e, context);
		ch->events++;
		count++;
		/* mainline gsi_evt_ring_doorbell and vendor gsi_incr_ring_wp: the event doorbell is the
		 * last element handed back, (index - 1) mod count (count - 1 at program time). Writing
		 * the next unused position itself leaves the ring with no free element: the second
		 * command then fails with ERROR_LOG type 3 code 5 (EVT_RING_EMPTY), seen in run f2. */
		gsi_wr(EV_DOORBELL(ch->event_id), (uint32_t)(ch->event_ring.phys +
		    ((ch->events + ch->slots - 1) % ch->slots) * ELEMENT_SIZE));
	}
	return count;
}

static bool
event_slot(const struct channel *ch, const struct event *e, uint32_t *slot)
{
	if (e->xfer < ch->tre_ring.phys || e->xfer >= ch->tre_ring.phys + ch->slots * ELEMENT_SIZE ||
	    ((e->xfer - ch->tre_ring.phys) & 0xf)) return false;
	*slot = (uint32_t)((e->xfer - ch->tre_ring.phys) / ELEMENT_SIZE);
	return true;
}

static bool
event_ok(const struct event *e)
{
	return e->code == GSI_EVT_SUCCESS || e->code == GSI_EVT_EOT || e->code == GSI_EVT_EOB;
}

struct command_done {
	uint64_t tre;
	bool done;
	uint8_t code;
};
static void
command_event(struct channel *ch, const struct event *e, void *context)
{
	struct command_done *waiting = context;
	if (e->xfer == waiting->tre) {
		waiting->done = true;
		waiting->code = e->code;
	}
	ch->done++;
}

static bool
ipa_command(const char *what, uint8_t opcode, const void *payload, size_t size)
{
	struct channel *ch = COMMAND;
	uint32_t slot = ch->next % ch->slots;
	struct command_done waiting = { .tre = ch->tre_ring.phys + slot * ELEMENT_SIZE };

	if (size > ch->buffer_size || ch->next - ch->done >= ch->slots - 1) return false;
	memcpy((void *)slot_virt(ch, slot), payload, size);
	dma_sync(slot_virt(ch, slot), size);
	tre_fill(ch, ch->next, slot_phys(ch, slot), opcode, TRE_TYPE_COMMAND | TRE_IEOT);
	ch->next++;
	channel_doorbell(ch);
	for (unsigned us = 0; us < 250000 && !waiting.done; us += 100) {
		channel_events(ch, command_event, &waiting);
		if (!waiting.done) {
			poll_quiet();
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	poll_end();
	if (!waiting.done || !event_ok(&(struct event){ .code = waiting.code })) {
		say("vayu-ipa-data: immediate command %s (%#x) failed: done %u code %#x state %u\n",
		    what, opcode, waiting.done, waiting.code, channel_state(ch, false));
		gsi_error(what);
		ch->errors++;
		return false;
	}
	say("vayu-ipa-data: immediate command %s completed (code %u)\n", what, waiting.code);
	return true;
}

static struct dma zero_rule, route_image, filter_image, header_image, proc_image;

static bool
table_images(void)
{
	uint64_t *route, *filter;
	uint32_t restricted = vayu_ipa_restricted_bytes();

	if (!pool_take(&zero_rule, 8, 128) || !pool_take(&route_image, 15 * 8, 128) ||
	    !pool_take(&filter_image, 9 * 8, 128) || !pool_take(&header_image, 0x140, 128) ||
	    !pool_take(&proc_image, 0x400, 128)) return false;
	route = (uint64_t *)route_image.virt;
	filter = (uint64_t *)filter_image.virt;
	for (unsigned i = 0; i < 15; i++) route[i] = zero_rule.phys;
	filter[0] = 0x3cfULL << 1;                /* vayu EP 0,1,2,3,6,7,8,9: eight filters */
	for (unsigned i = 1; i <= 8; i++) filter[i] = zero_rule.phys;
	for (unsigned i = 0; i < 5; i++) {
		struct dma *images[] = { &zero_rule, &route_image, &filter_image, &header_image, &proc_image };
		dma_sync(images[i]->virt, i == 0 ? 8 : i == 1 ? 15 * 8 : i == 2 ? 9 * 8 : i == 3 ? 0x140 : 0x400);
	}
	say("vayu-ipa-data: table images: zero rule %#llx, filter bitmap %#llx, restricted %#x\n",
	    (unsigned long long)zero_rule.phys, (unsigned long long)filter[0], restricted);
	return true;
}

static bool
table_init(const char *what, uint8_t opcode, bool filter, uint32_t hash_off, uint32_t nhash_off)
{
	struct { uint64_t hash, flags, nhash; } payload;
	struct dma *image = filter ? &filter_image : &route_image;
	uint32_t size = filter ? 9 * 8 : 15 * 8;
	uint32_t o = vayu_ipa_restricted_bytes();

	payload.hash = image->phys;
	payload.nhash = image->phys;
	payload.flags = (uint64_t)size | (uint64_t)(o + hash_off) << 12 |
	    (uint64_t)size << 28 | (uint64_t)(o + nhash_off) << 40;
	return ipa_command(what, opcode, &payload, sizeof(payload));
}

static bool
tables_setup(void)
{
	uint32_t o = vayu_ipa_restricted_bytes();
	struct { uint64_t source, flags; } header = {
		.source = header_image.phys,
		.flags = 0x140 | (uint64_t)(o + 0x688) << 12,
	};
	struct { uint64_t flags, source; } proc = {
		.flags = (uint64_t)0x400 << 16 | (uint64_t)(o + 0x7d0) << 32,
		.source = proc_image.phys,
	};

	if (!ipa_command("header table init", IPA_CMD_HDR_LOCAL, &header, sizeof(header)) ||
	    !ipa_command("header processing contexts", IPA_CMD_DMA_SHARED, &proc, sizeof(proc)) ||
	    !table_init("IPv4 route table init", IPA_CMD_V4_ROUTE, false, 0x488, 0x508) ||
	    !table_init("IPv6 route table init", IPA_CMD_V6_ROUTE, false, 0x588, 0x608) ||
	    !table_init("IPv4 filter table init", IPA_CMD_V4_FILTER, true, 0x288, 0x308) ||
	    !table_init("IPv6 filter table init", IPA_CMD_V6_FILTER, true, 0x388, 0x408)) return false;
	ipa_wr(IPA_LOCAL_PROC_BASE, o + 0x7d0);
	ipa_wr(IPA_HASH_FLUSH, 0x1111);
	return true;
}

static bool
tables_verify(void)
{
	/* The readback window must be the same SRAM the immediate commands targeted:
	 * vayu_ipa_sram=1 moves it to 0x1e47000 (+restricted) for IPA 4.1. */
	uint32_t base = vayu_ipa_sram_base();
	vm_offset_t sram = ml_io_map_unmappable(base, IPA_SRAM_SIZE, VM_WIMG_IO);
	uint32_t o = vayu_ipa_restricted_bytes();
	bool ok = sram != 0;

	if (!ok) {
		say("vayu-ipa-data: SRAM readback window %#x did not map\n", base);
		return false;
	}
#define EXPECT64(off, value) do { \
	uint64_t got = sram_rd64(sram, (off)); \
	if (got != (uint64_t)(value)) { \
		say("vayu-ipa-data: SRAM %#x = %#llx, expected %#llx\n", (off), \
		    (unsigned long long)got, (unsigned long long)(uint64_t)(value)); \
		ok = false; \
	} \
} while (0)
	EXPECT64(o + 0x488, zero_rule.phys);
	EXPECT64(o + 0x508, zero_rule.phys);
	EXPECT64(o + 0x288, 0x3cfULL << 1);
	EXPECT64(o + 0x290, zero_rule.phys);
	EXPECT64(o + 0x308, 0x3cfULL << 1);
	EXPECT64(o + 0x310, zero_rule.phys);
	EXPECT64(o + 0x688, 0);
	EXPECT64(o + 0x7d0, 0);
#undef EXPECT64
	if (ok) say("vayu-ipa-data: SRAM route/filter/header/proc table readback PASSED\n");
	return ok;
}

static void
transmit_event(struct channel *ch, const struct event *e, void *context __unused)
{
	uint32_t slot;
	if (!event_slot(ch, e, &slot)) {
		ch->errors++;
		return;
	}
	if (slot != ch->done % ch->slots || !event_ok(e)) ch->errors++;
	ch->done++;
}

static void
receive_event(struct channel *ch, const struct event *e, void *context __unused)
{
	uint32_t slot;
	if (!event_slot(ch, e, &slot)) {
		ch->errors++;
		ch->done++;                            /* one bad event must not pin the ring forever */
		return;
	}
	if (slot != ch->done % ch->slots || !event_ok(e)) ch->errors++;
	ch->done++;
	uint32_t length = e->len > ch->buffer_size ? ch->buffer_size : e->len;
	const uint8_t *buffer = (const uint8_t *)slot_virt(ch, slot);
	dma_sync((vm_offset_t)buffer, length);
	ch->bytes += length;
	if (trace) say("vayu-ipa-data: %s slot %u: %u bytes code %u\n", ch->name, slot, length, e->code);
	if (ch->qmap) {
		const uint8_t *cursor = buffer;
		size_t remaining = length;
		while (remaining) {
			struct vayu_qmap_header header;
			const uint8_t *packet;
			size_t packet_length;
			if (!vayu_qmap_next(&cursor, &remaining, &header, &packet, &packet_length)) {
				ch->dropped++;
				break;
			}
			if (header.command) {
				qmap_commands++;
				if (qmap_commands == 1) {
					say("vayu-qmap: command frame mux %u, %u bytes dropped; no MAP "
					    "command handling (checksum offload is off)\n", header.mux_id,
					    (unsigned)packet_length);
				}
				ch->dropped++;
				continue;
			}
			if (header.next_header) {
				/* A next header only exists with checksum offload; WDA asked for none. */
				if (trace) {
					say("vayu-qmap: mux %u has a next header; dropped\n", header.mux_id);
				}
				ch->dropped++;
				continue;
			}
			if (header.mux_id != VAYU_CELL_NET_MUX) {
				qmap_unknown_mux++;
				if (qmap_unknown_mux == 1 || trace) {
					say("vayu-qmap: mux %u is not the bound mux %u; dropped\n",
					    header.mux_id, VAYU_CELL_NET_MUX);
				}
				ch->dropped++;
				continue;
			}
			if (!qmap_rx_logged) {
				qmap_rx_logged = true;
				say("vayu-qmap: first data frame mux %u, %u bytes -> %s\n", header.mux_id,
				    (unsigned)packet_length, vayu_cell_net_name());
			}
			ch->packets++;
			first_packet("received", ch->name, (uint32_t)packet_length);
			/* ifnet_input may generate an ICMP/TCP reply and re-enter transmit. Do not
			 * hold the IPA lock across it; only this thread consumes this RX channel. */
			lck_mtx_unlock(&ipa_data_lock);
			vayu_cell_net_input(header.mux_id, packet, packet_length);
			lck_mtx_lock(&ipa_data_lock);
		}
	} else {
		ch->packets++;                            /* LAN exceptions are deliberately dropped. */
	}
	receive_post(ch);
	channel_doorbell(ch);
}

bool
vayu_ipa_data_transmit(uint8_t mux, const void *packet, size_t length)
{
	struct channel *ch = WAN_TX;
	struct vayu_qmap_header header = { .mux_id = mux };
	uint32_t slot, pad;
	uint8_t *buffer;
	bool accepted = false;

	if (!ready || packet == NULL || length == 0 || length > 1500) return false;
	pad = (4 - (length & 3)) & 3;
	header.pad_len = (uint8_t)pad;
	header.length = (uint16_t)(length + pad);
	if (length + pad + VAYU_QMAP_HEADER_LEN > ch->buffer_size) return false;
	lck_mtx_lock(&ipa_data_lock);
	channel_events(ch, transmit_event, NULL);
	if (!ready || ch->next - ch->done >= ch->slots - 1) {
		ch->dropped++;
		goto out;
	}
	slot = ch->next % ch->slots;
	buffer = (uint8_t *)slot_virt(ch, slot);
	if (!vayu_qmap_header_encode(buffer, &header)) goto out;
	memcpy(buffer + VAYU_QMAP_HEADER_LEN, packet, length);
	memset(buffer + VAYU_QMAP_HEADER_LEN + length, 0, pad);
	dma_sync((vm_offset_t)buffer, VAYU_QMAP_HEADER_LEN + length + pad);
	tre_fill(ch, ch->next, slot_phys(ch, slot), (uint16_t)(VAYU_QMAP_HEADER_LEN + length + pad),
	    TRE_TYPE_XFER | TRE_IEOT | TRE_BEI);
	ch->next++;
	channel_doorbell(ch);
	ch->packets++;
	ch->bytes += length;
	first_packet("sent", ch->name, (uint32_t)length);
	accepted = true;
out:
	lck_mtx_unlock(&ipa_data_lock);
	return accepted;
}

static void
ipa_data_thread(void *argument __unused, wait_result_t wait __unused)
{
	while (!stopping) {
		if (ready) {
			lck_mtx_lock(&ipa_data_lock);
			channel_events(WAN_RX, receive_event, NULL);
			channel_events(LAN_RX, receive_event, NULL);
			channel_events(WAN_TX, transmit_event, NULL);
			lck_mtx_unlock(&ipa_data_lock);
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	thread_running = false;
	thread_terminate(current_thread());
}

static bool
channel_memory(void)
{
	for (unsigned i = 0; i < NCHANNELS; i++) {
		struct channel *ch = &channels[i];
		ch->next = ch->done = ch->events = 0;
		ch->event_allocated = ch->allocated = ch->started = false;
		ch->packets = ch->bytes = ch->dropped = ch->errors = 0;
		if (!pool_take(&ch->tre_ring, ch->slots * ELEMENT_SIZE, ch->slots * ELEMENT_SIZE) ||
		    !pool_take(&ch->event_ring, ch->slots * ELEMENT_SIZE, ch->slots * ELEMENT_SIZE) ||
		    !pool_take(&ch->data, ch->slots * ch->buffer_size, PAGE_SIZE)) return false;
	}
	return true;
}

static bool
registers_map_and_check(void)
{
	ipa_regs = ml_io_map_unmappable(IPA_PHYS, IPA_SIZE, VM_WIMG_IO);
	ipa_debug = ml_io_map_unmappable(IPA_DEBUG_PHYS, IPA_DEBUG_SIZE, VM_WIMG_IO);
	gsi_regs = ml_io_map_unmappable(GSI_PHYS, GSI_SIZE, VM_WIMG_IO);
	if (!ipa_regs || !ipa_debug || !gsi_regs) {
		say("vayu-ipa-data: cannot map IPA/debug/GSI windows\n");
		return false;
	}
	uint32_t status = gsi_rd(GSI_STATUS), version = gsi_rd(GSI_SW_VERSION), params = gsi_rd(GSI_HW_PARAM_2);
	uint32_t channels_hw = (params >> 3) & 0x1f;
	max_event_rings = (params >> 8) & 0x1f;
	uint32_t pipes = ipa_rd(IPA_ENABLED_PIPES);
	say("vayu-ipa-data: GSI status %#x version %#x params %#x: %u channels %u events; IPA has %u pipes\n",
	    status, version, params, channels_hw, max_event_rings, pipes);
	/* IPA_ENABLED_PIPES is a COUNT of pipes (stock ipa3_get_num_pipes() returns it as is:
	 * ipa_utils.c:7075), not a bitmask. 115037 read 0x17 = 23 pipes. Accept the count form
	 * (>11 means endpoints 0..11 exist) and, defensively, the old bitmask form (bits 2/5/10/11)
	 * so a firmware that reports either is not refused. */
	{
		bool pipes_ok = pipes > 11 || (pipes & 0xc24u) == 0xc24u;

		if (!(status & 1) || version == 0 || version == ~0u || channels_hw <= 6 ||
		    max_event_rings < NCHANNELS || !pipes_ok) {
			say("vayu-ipa-data: GSI 2.0 preflight failed (status %#x, %u channels, %u event rings, "
			    "pipes %#x as %s); no channel writes\n", status, channels_hw, max_event_rings, pipes,
			    pipes > 11 ? "count" : "mask");
			say("vayu-data: S0 GSI: NOT READY\n");
			return false;
		}
	}
	say("vayu-data: S0 GSI: READY status %#x fw %#x, %u channels, %u event rings, %u pipes\n",
	    status, version, channels_hw, max_event_rings, pipes);
	return true;
}

static bool
channels_down(void)
{
	bool ok = true;

	for (unsigned i = NCHANNELS; i-- > 0;) {
		struct channel *ch = &channels[i];
		if (ch->started) {
			if (!gsi_command(ch, false, GSI_STOP, 3)) {
				say("vayu-ipa-data: %s would not stop; leaving its SMMU/context allocated\n", ch->name);
				ok = false;
				continue;
			}
			ch->started = false;
		}
		if (ch->allocated) {
			if (gsi_command(ch, false, GSI_RESET, 1)) gsi_command(ch, false, GSI_DEALLOCATE, 0);
			ch->allocated = false;
		}
		if (ch->event_allocated) {
			if (gsi_command(ch, true, GSI_RESET, 1)) gsi_command(ch, true, GSI_DEALLOCATE, 0);
			ch->event_allocated = false;
		}
	}
	return ok;
}

bool
vayu_ipa_data_prepare(void)
{
	uint32_t enabled = 0;

	if (prepared) return ready;
	if (!PE_parse_boot_argn("vayu_ipa_data", &enabled, sizeof(enabled)) || enabled == 0) return true;
	if (enabled != 1) {
		say("vayu-ipa-data: vayu_ipa_data=%u refused: only 1 enables GSI\n", enabled);
		return false;
	}
	PE_parse_boot_argn("vayu_ipa_data_trace", &trace, sizeof(trace));
	PE_parse_boot_argn("vayu_ipa_data_trace_ms", &mmio_trace_ms, sizeof(mmio_trace_ms));
	PE_parse_boot_argn("vayu_gsi_mainline", &gsi_mainline, sizeof(gsi_mainline));
	if (trace & 1) say("vayu-ipa-data: vayu_ipa_data_trace=%u: every GSI/IPA access from the cmd channel "
	    "on is said first, then %u ms\n", trace, mmio_trace_ms);
	if (gsi_mainline != 0) {
		say("vayu-ipa-data: vayu_gsi_mainline=%u: mainline sequence %s, DOORBELL_1 %s, GSI_CFG1 %s\n",
		    gsi_mainline, gsi_mainline & GSI_ML_SEQUENCE ? "on" : "off",
		    gsi_mainline & (GSI_ML_SEQUENCE | GSI_ML_NO_DB1) ? "not written" : "written",
		    gsi_mainline & (GSI_ML_SEQUENCE | GSI_ML_NO_CFG1) ? "not read" : "read");
	}
	stopping = false;
	filter_failed = false;
	ready = false;
	/* Empty-table initialization covers modem-owned areas too. It is legal only
	 * before MPSS; never rewrite them after the modem accepted INIT_MODEM_DRIVER. */
	if (vayu_ipa_modem_started()) {
		say("vayu-ipa-data: MPSS already started; refusing to rewrite modem-owned tables\n");
		return false;
	}
	/* Small table images first: allocating the large packet buffers first would
	 * waste the last pool bytes on PAGE_SIZE alignment before the zero rule. */
	vayu_ipa_data_step("data: map and read GSI_STATUS/SW_VERSION/HW_PARAM_2, IPA_ENABLED_PIPES");
	if (!registers_map_and_check()) return false;
	vayu_ipa_data_step("data: DMA pool, table images, channel memory (no MMIO)");
	if (!pool_init() || !table_images() || !channel_memory()) return false;
	/* The whole pool was zeroed through the cacheable CPU mapping. Push those dirty
	 * zero lines out before any doorbell: a later dc civac on a device-written event
	 * or RX buffer must never write stale zeros over the hardware's fresh data. */
	dma_sync((vm_offset_t)ipa_pool, sizeof(ipa_pool));
	vayu_ipa_data_step("data: GSI interrupts off");
	gsi_interrupts_off();
	mmio_seq = 0;
	mmio_trace = (trace & 1) != 0;            /* from here to the table readback; off on every exit */
	for (unsigned i = 0; i < NCHANNELS; i++) {
		vayu_ipa_data_step(channels[i].name);   /* (channel_setup: event ring, channel, endpoint) */
		if (!channel_setup(&channels[i])) {
			(void)channels_down();
			mmio_trace = false;
			return false;
		}
	}
	for (unsigned i = 0; i < NCHANNELS; i++) {
		vayu_ipa_data_step("data: channel start (GSI START, RX post, doorbell)");
		if (!channel_start(&channels[i])) {
			(void)channels_down();
			mmio_trace = false;
			return false;
		}
	}
	vayu_ipa_data_step("data: table init commands through APPS_CMD_PROD, then read back");
	if (!tables_setup() || !tables_verify()) {
		say("vayu-ipa-data: tables are not initialized; stopping channels, no data_ready\n");
		(void)channels_down();
		mmio_trace = false;
		return false;
	}
	if (mmio_trace) say("vayu-ipa-data: mmio trace off after #%u accesses\n", mmio_seq);
	mmio_trace = false;
	thread_t thread;
	thread_running = true;                    /* readiness cannot race the new thread */
	if (kernel_thread_start(ipa_data_thread, NULL, &thread) != KERN_SUCCESS) {
		thread_running = false;
		say("vayu-ipa-data: no polling thread\n");
		(void)channels_down();
		return false;
	}
	thread_deallocate(thread);
	prepared = true;
	ready = true;
	say("vayu-ipa-data: GSI/tables READY; runtime modem filter rules still fail closed; no internet claim\n");
	say("vayu-data: S2 GSI CHANNELS: OK 4 channels (cmd ch4/EP5, lan-rx ch5/EP10, wan-tx ch3/EP2, "
	    "wan-rx ch6/EP11), event rings, tables read back\n");
	return true;
}

bool
vayu_ipa_data_ready(void)
{
	return prepared && ready && thread_running && !filter_failed && !stopping;
}

void
vayu_ipa_data_filter_error(void)
{
	if (!filter_failed) say("vayu-ipa-data: modem requested unimplemented filter installation; WDS/data blocked\n");
	filter_failed = true;
	ready = false;
}

bool
vayu_ipa_data_stop(void)
{
	bool ok;

	if (!prepared && !thread_running) return true;
	stopping = true;
	ready = false;
	for (unsigned ms = 0; ms < 2000 && thread_running; ms += 10) delay_for_interval(10, NSEC_PER_MSEC);
	if (thread_running) {
		say("vayu-ipa-data: polling thread did not stop; IPA firmware must stay up\n");
		return false;
	}
	ok = channels_down();
	prepared = !ok;
	say("vayu-ipa-data: %s\n", ok ? "stopped" : "channel stop failed; IPA firmware must stay up");
	return ok;
}
