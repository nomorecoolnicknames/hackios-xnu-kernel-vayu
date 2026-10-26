/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X3 Pro (vayu, SM8150-AC).
 * Only with boot-args cpufreq=1 cpufreq_vayu=1: the addresses are this phone's.
 *
 * hakios.cpufreq.c does the same job on the POCO X5 Pro (SM7325). Its hardware ("EPSS") has three
 * groups of 0x1000 bytes at 0x18591000, a table of 4-byte rows at 0x100 and the row that is wanted at
 * 0x320. SM8150 uses the older "OSM" (operating state manager): three groups of 0x1400 bytes at
 * 0x18323000 (the four small cores), 0x18325800 (three big ones) and 0x18327800 (the prime core); in
 * each group a table of 40 rows of 32 bytes at 0x110 -- the frequency in the row's first word, the
 * voltage in the second -- and the row that is wanted at 0x920, one word per core of the group when
 * per-core DCVS is on (0xbc, bit 0). Linux: drivers/clk/qcom/clk-cpu-osm.c of the phone's own kernel
 * (MiCode/Xiaomi_Kernel_OpenSource, branch vayu-r-oss) and drivers/cpufreq/qcom-cpufreq-hw.c for
 * "qcom,cpufreq-hw" (the non-EPSS table: 0x110/32-byte rows/0x920). The device tree names the three
 * bases in "qcom,cpucc" (osm_pwrcl_base, osm_perfcl_base, osm_perfpcl_base); it has no cpufreq node.
 *
 * A row's frequency word: bits [31:30] the clock source, bits [7:0] the multiplier of the 19.2 MHz
 * crystal; source 0 is not a multiple but the fixed 300 MHz (Linux's OSM_INIT_RATE, its "alternate"
 * GPLL0 / 2). Bits
 * [18:16] are the row's core count: 1 marks a single-core ("turbo") row, which Linux treats as a
 * boost frequency and does not use for all cores. So the table is the hardware's, not the device
 * tree's, and its order is not to be assumed: this driver takes the fastest ordinary row it finds,
 * never a row slower than the one the firmware left, and writes nothing unless the group says it is
 * enabled. With cpufreq_dry=1 it writes nothing at all and only says what it sees; with
 * cpufreq_turbo=1 single-core rows are allowed too.
 *
 * The system cache's clock (L3) is a fourth OSM group at 0x18321000 with the same table and row
 * register (vendor clk-cpu-osm.c:985-992,44-53,935-972,1170,245); cpufreq_l3=0 leaves it alone. With
 * vayu_ddr=1 the driver also votes for memory bandwidth on the RPMH bus clock managers MC0, SH0 and
 * ACV (vayu_ddr_vote=N, default the most, 16383; the vote is not taken back on the way out).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vayu_cpufreq.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern bool hakios_spmi_peek(uint16_t id, uint8_t reg, uint8_t *value);
extern bool vayu_rpmh_tcs_lock(void);
extern void vayu_rpmh_tcs_unlock(bool held);

#define OSM_DOMAINS             3
#define OSM_DOMAIN_SIZE         0x1400
#define OSM_ENABLE              0x000           /* bit 0: the group's clock hardware is enabled */
#define OSM_CORE_DCVS           0x0bc           /* bit 0: a row word per core, not one for the group */
#define OSM_FREQ_LUT            0x110           /* 40 rows, 32 bytes each; frequency in the first word */
#define OSM_VOLT_LUT            0x114           /* ... and the voltage (mV, low 12 bits) in the second */
#define OSM_PERF_STATE          0x920           /* the row that is wanted, then +4 bytes per core */
#define OSM_ROW_SIZE            32
#define OSM_ROWS                40
#define OSM_SINGLE_CORE         1               /* the core-count field of a single-core (turbo) row */

/*
 * The system cache's own OSM group: the phone's device tree in "qcom,cpucc" has
 * reg = <0x18321000 0x1400 ...> (osm_l3_base; vendor clk-cpu-osm.c:985-992), and l3_clk reads its
 * table with clk_osm_read_lut() too (clk-cpu-osm.c:935-972,1170). The table is the cores'
 * (FREQ_REG 0x110 / VOLT_REG 0x114, 32-byte rows, 40 rows; clk-cpu-osm.c:44-53), and its rate is
 * the row index written to DCVS_PERF_STATE_DESIRED_REG(0) = 0x920 (l3_clk_set_rate(),
 * clk-cpu-osm.c:245). The vendor L3 table takes every row up to the first repeat with no core-count
 * filter (l.964-972), so the fastest row of any core count is the one to take.
 */
#define OSM_L3_PHYS             0x18321000      /* qcom,cpucc "osm_l3_base"; clk-cpu-osm.c:985-992 */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: GPLL0 / 2, a fixed frequency */

static const uint32_t osm_phys[OSM_DOMAINS] = { 0x18323000, 0x18325800, 0x18327800 };
static const uint32_t osm_cores[OSM_DOMAINS] = { 4, 3, 1 };     /* cpu@0-3, cpu@4-6, cpu@7 in vayu.dtb */

static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);                  /* (the whole line under the log's lock: not mixed with other processors') */
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

/* How fast a simple loop runs, in thousands of iterations per millisecond (as hakios.cpufreq.c). */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

struct osm_row {
	uint32_t index, khz, volt, cores;
};

static void
read_row(vm_offset_t base, uint32_t index, struct osm_row *row)
{
	uint32_t freq = *(volatile uint32_t *)(base + OSM_FREQ_LUT + index * OSM_ROW_SIZE);
	uint32_t volt = *(volatile uint32_t *)(base + OSM_VOLT_LUT + index * OSM_ROW_SIZE);

	row->index = index;
	row->khz = (freq >> 30) ? XO_KHZ * (freq & 0xff) : ALTERNATE_KHZ;
	row->volt = volt & 0xfff;
	row->cores = (freq >> 16) & 0x7;
}

/*
 * The fastest row of a group that is an ordinary operating point (or any row with turbo allowed).
 * The table ends by repeating its last row -- which, for a row whose source is 0, also looks like
 * 300 MHz -- so the scan stops at the first repeat; it does not assume the rows are in any order.
 */
static bool
top_row(vm_offset_t base, bool turbo, bool show, struct osm_row *top, uint32_t *rows)
{
	uint32_t previous = 0;

	top->index = 0;
	top->khz = 0;
	top->volt = 0;
	top->cores = 0;
	*rows = 0;
	for (uint32_t i = 0; i < OSM_ROWS; i++) {
		struct osm_row row;

		read_row(base, i, &row);
		if (show) {
			tell("cpufreq: vayu:   row %2u: %4u MHz, %4u mV, %u core(s)%s\n", i, row.khz / 1000, row.volt, row.cores,
			    row.cores == OSM_SINGLE_CORE ? ", single core" : "");
		}
		if (i > 0 && row.khz == previous) {
			break;                  /* the table ends by repeating its last row */
		}
		previous = row.khz;
		*rows = i + 1;
		if (row.cores == OSM_SINGLE_CORE && !turbo) {
			continue;               /* a boost row: not a frequency for every core of the group */
		}
		if (row.khz > top->khz) {
			*top = row;
		}
	}
	/* A table of only 300 MHz rows says nothing useful; writing it could only slow a core down. */
	return top->khz > ALTERNATE_KHZ;
}

struct raised_domain {
	vm_offset_t base;
	uint32_t was;
	uint32_t row[3];        /* by thermal level (vayu_thermal below): the top row, a ~3/4 row, the slowest row */
	uint32_t khz[3];
	bool per_core;
	bool wrote;
};

static struct raised_domain raised[OSM_DOMAINS];

/*
 * The rows the thermal throttle (vayu_thermal below) steps through: level 0 the top row just found,
 * level 1 the fastest ordinary row at no more than 3/4 of its frequency, level 2 the slowest ordinary
 * row of the table. (If no row fits level 1, it is level 2's.)
 */
static void
thermal_rows(vm_offset_t base, uint32_t rows, bool turbo, const struct osm_row *top, uint32_t row[3], uint32_t khz[3])
{
	struct osm_row mid = { 0 }, low = { .khz = UINT32_MAX };

	for (uint32_t i = 0; i < rows; i++) {
		struct osm_row r;

		read_row(base, i, &r);
		if (r.cores == OSM_SINGLE_CORE && !turbo) {
			continue;
		}
		if (r.khz <= top->khz / 4 * 3 && r.khz > mid.khz) {
			mid = r;
		}
		if (r.khz < low.khz) {
			low = r;
		}
	}
	if (low.khz == UINT32_MAX) {
		low = *top;
	}
	if (mid.khz == 0) {
		mid = low;
	}
	row[0] = top->index;
	khz[0] = top->khz;
	row[1] = mid.index;
	khz[1] = mid.khz;
	row[2] = low.index;
	khz[2] = low.khz;
}

static void
raise_domain(uint32_t domain, uint32_t cores, bool dry, bool force, bool turbo)
{
	vm_offset_t base = ml_io_map(osm_phys[domain], OSM_DOMAIN_SIZE);
	struct osm_row top, now;
	uint32_t enable, dcvs, state, rows;
	bool have_now;

	enable = *(volatile uint32_t *)(base + OSM_ENABLE);
	dcvs = *(volatile uint32_t *)(base + OSM_CORE_DCVS);
	state = *(volatile uint32_t *)(base + OSM_PERF_STATE);
	tell("cpufreq: vayu: group %u at 0x%x (%u of our cores): %s, per-core DCVS %s, row %u\n", domain, osm_phys[domain],
	    cores, (enable & 1) ? "enabled" : "NOT enabled", (dcvs & 1) ? "on" : "off", state);
	if (!top_row(base, turbo, dry, &top, &rows)) {
		tell("cpufreq: vayu: group %u: no usable row in the table, not touching it\n", domain);
		return;
	}
	tell("cpufreq: vayu: group %u: %u rows, fastest ordinary row %u, %u MHz, %u mV\n", domain, rows, top.index,
	    top.khz / 1000, top.volt);
	have_now = state < rows;
	if (have_now) {
		read_row(base, state, &now);
	}
	if (!(enable & 1) && !force) {
		tell("cpufreq: vayu: group %u: the hardware says it is not enabled, not touching it (cpufreq_force=1: anyway)\n",
		    domain);
		return;
	}
	if (!have_now && !force) {
		tell("cpufreq: vayu: group %u: row %u is outside the table, not touching it (cpufreq_force=1: anyway)\n", domain,
		    state);
		return;
	}
	if (have_now && top.khz <= now.khz) {
		tell("cpufreq: vayu: group %u: row %u (%u MHz) is not faster than row %u (%u MHz), not touching it\n", domain,
		    top.index, top.khz / 1000, state, now.khz / 1000);
		return;
	}
	if (dry) {
		tell("cpufreq: vayu: group %u: read-only: would take row %u (%u MHz)%s\n", domain, top.index, top.khz / 1000,
		    have_now ? "" : ", the current row is outside the table");
		return;
	}
	raised[domain].base = base;
	raised[domain].was = state;
	raised[domain].per_core = (dcvs & 1) != 0;
	thermal_rows(base, rows, turbo, &top, raised[domain].row, raised[domain].khz);
	raised[domain].wrote = true;
	*(volatile uint32_t *)(base + OSM_PERF_STATE) = top.index;
	if (raised[domain].per_core) {
		for (uint32_t core = 1; core < osm_cores[domain]; core++) {
			*(volatile uint32_t *)(base + OSM_PERF_STATE + 4 * core) = top.index;
		}
	}
	delay_for_interval(20, NSEC_PER_MSEC);
	__asm__ volatile ("dsb sy" ::: "memory");
	state = *(volatile uint32_t *)(base + OSM_PERF_STATE);
	tell("cpufreq: vayu: group %u: row %u -> %u, %u MHz%s\n", domain, raised[domain].was, top.index, top.khz / 1000,
	    state == top.index ? "" : " (the hardware reports another row)");
}

/*
 * The system cache's clock: the same table and the same row register as a core group, but at
 * 0x18321000 and with no cores of its own -- every row counts, single-core ones too
 * (clk-cpu-osm.c:964-972). The vendor never gates this group on its enable register (0x0 is only
 * read as a barrier, clk-cpu-osm.c:116), so neither do we; the bit goes to the log only.
 */
static struct {
	vm_offset_t base;
	uint32_t was, top;
	bool wrote;
} l3_raised;

static void
raise_l3(bool dry)
{
	vm_offset_t base = ml_io_map(OSM_L3_PHYS, OSM_DOMAIN_SIZE);
	struct osm_row top, now;
	uint32_t enable, state, rows;
	bool have_now, have_top;

	enable = *(volatile uint32_t *)(base + OSM_ENABLE);
	state = *(volatile uint32_t *)(base + OSM_PERF_STATE);
	have_top = top_row(base, true /* any core count */, dry, &top, &rows);
	if (state < OSM_ROWS) {
		read_row(base, state, &now);
	} else {
		memset(&now, 0, sizeof(now));
	}
	have_now = state < rows;
	tell("cpufreq: vayu: L3 at 0x18321000: row %u (%u MHz), %u rows, fastest row %u, %u MHz\n", state,
	    now.khz / 1000, rows, top.index, top.khz / 1000);
	tell("cpufreq: vayu: L3: enable bit %u (the vendor only reads it, clk-cpu-osm.c:116)\n", enable & 1);
	if (!have_top) {
		tell("cpufreq: vayu: L3: no usable row in the table, not touching it\n");
		return;
	}
	if (have_now && top.khz <= now.khz) {
		tell("cpufreq: vayu: L3: row %u (%u MHz) is not faster than row %u (%u MHz), not touching it\n", top.index,
		    top.khz / 1000, state, now.khz / 1000);
		return;
	}
	if (!have_now) {
		tell("cpufreq: vayu: L3: row %u is outside the table, not touching it\n", state);
		return;
	}
	if (dry) {
		tell("cpufreq: vayu: L3: read-only: would take row %u (%u MHz)\n", top.index, top.khz / 1000);
		return;
	}
	l3_raised.base = base;
	l3_raised.was = state;
	l3_raised.top = top.index;
	l3_raised.wrote = true;
	*(volatile uint32_t *)(base + OSM_PERF_STATE) = top.index;
	delay_for_interval(20, NSEC_PER_MSEC);
	__asm__ volatile ("dsb sy" ::: "memory");
	state = *(volatile uint32_t *)(base + OSM_PERF_STATE);
	if (state == top.index) {
		tell("cpufreq: vayu: L3: row %u -> %u, %u MHz\n", l3_raised.was, top.index, top.khz / 1000);
	} else {
		tell("cpufreq: vayu: L3: row %u -> %u, %u MHz (the hardware reports another row %u)\n", l3_raised.was,
		    top.index, top.khz / 1000, state);
	}
}

/*
 * Boot-arg vayu_ddr=1: a bandwidth vote through RPMh, as hakios.cpufreq.c does. The vendor's
 * CPU->LLCC and LLCC->DDR bandwidth devices run their governor at "performance"
 * (sm8150.dtsi:1014-1055), and their BCMs are MC0 (DDR), SH0 (the shared hub in front of the
 * system cache) and ACV, all on the apps RSC (sm8150-bus.dtsi:57-83). The ACV vote from apps is
 * bit 3, 0x8 (msm_bus_arb_rpmh.c:31 BCM_TCS_CMD_ACV_APPS, used at l.501-502; sm8150.c:1283-1285).
 */
#define VAYU_CMD_DB_PHYS        0x85f20000UL    /* mainline sm8150.dtsi; boot-arg vayu_cmd_db */
#define VAYU_CMD_DB_SIZE        0x20000UL
#define VAYU_CMD_DB_BCM         5               /* the kind of resource in a slave's id: bus clock managers (cmd-db.h:14) */

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

/*
 * The BCM command word: commit bit 30, valid bit 29, vote_x bits 27:14 and vote_y bits 13:0
 * (msm_bus_fabric_rpmh.c:30-44); 0x3fff is the largest value a field holds.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define BCM_ACV_APPS            8               /* msm_bus_arb_rpmh.c:31; sm8150.c:1283-1285 */

/*
 * The apps RSC's first command slot set (DRV 2; vendor sm8150.dtsi:2597-2607,
 * reg = <0x18220000 0x100>, <0x18220d00 0x3000>, first TCS an ACTIVE one). The offsets and the
 * polling are vayu_qup_spi.c:1983-2115, which has worked on this phone.
 */
#define VAYU_RSC_TCS_PHYS       0x18220d00UL    /* vendor sm8150.dtsi:2597-2607; vayu_qup_spi.c:1983 */
#define VAYU_RSC_TCS_SIZE       0x1000UL
#define TCS_IRQ_ENABLE          0x00            /* vayu_qup_spi.c:1985 */
#define TCS_IRQ_STATUS          0x04            /* vayu_qup_spi.c:1986 */
#define TCS_IRQ_CLEAR           0x08            /* vayu_qup_spi.c:1987 */
#define TCS_WAIT_FOR_CMPL       0x10            /* vayu_qup_spi.c:1988 */
#define TCS_CONTROL             0x14            /* vayu_qup_spi.c:1989 */
#define TCS_AMC_ENABLE          (1u << 16)      /* vayu_qup_spi.c:1990 */
#define TCS_AMC_TRIGGER         (1u << 24)      /* vayu_qup_spi.c:1991 */
#define TCS_STATUS              0x18            /* per TCS: nonzero = idle (vayu_qup_spi.c:1992) */
#define TCS_CMD_ENABLE          0x1c            /* vayu_qup_spi.c:1993 */
#define TCS_CMD_MSGID(n)        (0x30 + 20 * (n))       /* vayu_qup_spi.c:1994 */
#define TCS_CMD_ADDR(n)         (0x34 + 20 * (n))       /* vayu_qup_spi.c:1995 */
#define TCS_CMD_DATA(n)         (0x38 + 20 * (n))       /* vayu_qup_spi.c:1996 */
#define TCS_CMD_STATUS(n)       (0x3c + 20 * (n))       /* vayu_qup_spi.c:1997 */
#define CMD_MSGID_WRITE_4       0x10108         /* len 8 | RESP_REQ bit 8 | WRITE bit 16 (rpmh-rsc.c v6.18:89-91) */

static uint32_t
cmd_db_address(const struct cmd_db_header *db, const char *name)
{
	char id[8] = { 0 };

	memcpy(id, name, strnlen(name, sizeof(id)));   /* (a cmd-db id fills all 8 bytes with no NUL: "lnbclka2"; strlcpy kept 7) */
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry;

		if (slave->id != VAYU_CMD_DB_BCM || slave->header_offset >= VAYU_CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* One active-only request in command slot set 0, and the wait until RPMh has done it (as vayu_qup_spi.c). */
static bool
rpmh_vote(const uint32_t *address, const uint32_t *data, unsigned count, uint32_t *status)
{
	vm_offset_t tcs = ml_io_map(VAYU_RSC_TCS_PHYS, VAYU_RSC_TCS_SIZE);
	uint32_t irq_enable;
	bool idle = false, done = false;

	if (tcs == 0) {
		tell("cpufreq: vayu: ddr: cannot map the RSC\n");
		return false;
	}
	irq_enable = *(volatile uint32_t *)(tcs + TCS_IRQ_ENABLE);
	tell("cpufreq: vayu: ddr: tcs0 status %#x control %#x irq en %#x st %#x cmd_en %#x\n",
	    *(volatile uint32_t *)(tcs + TCS_STATUS), *(volatile uint32_t *)(tcs + TCS_CONTROL), irq_enable,
	    *(volatile uint32_t *)(tcs + TCS_IRQ_STATUS), *(volatile uint32_t *)(tcs + TCS_CMD_ENABLE));
	for (unsigned i = 0; i < 2000; i++) {
		if (*(volatile uint32_t *)(tcs + TCS_STATUS) != 0) {
			idle = true;
			break;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	if (!idle) {
		/* somebody else's request is in flight: touch nothing of theirs */
		tell("cpufreq: vayu: ddr: RPMh: the command slots stay busy; not voting\n");
		return false;
	} else {
		*(volatile uint32_t *)(tcs + TCS_CONTROL) &= ~TCS_AMC_TRIGGER;
		*(volatile uint32_t *)(tcs + TCS_CONTROL) &= ~TCS_AMC_ENABLE;
		*(volatile uint32_t *)(tcs + TCS_IRQ_CLEAR) = 1;
		for (unsigned i = 0; i < count; i++) {
			*(volatile uint32_t *)(tcs + TCS_CMD_MSGID(i)) = CMD_MSGID_WRITE_4;
			*(volatile uint32_t *)(tcs + TCS_CMD_ADDR(i)) = address[i];
			*(volatile uint32_t *)(tcs + TCS_CMD_DATA(i)) = data[i];
		}
		*(volatile uint32_t *)(tcs + TCS_WAIT_FOR_CMPL) = (1u << count) - 1;     /* the three commands: 0x7 */
		*(volatile uint32_t *)(tcs + TCS_CMD_ENABLE) = (1u << count) - 1;
		*(volatile uint32_t *)(tcs + TCS_IRQ_ENABLE) = irq_enable | 1;  /* status follows the enable; nothing routes the interrupt */
		*(volatile uint32_t *)(tcs + TCS_CONTROL) = TCS_AMC_ENABLE;
		*(volatile uint32_t *)(tcs + TCS_CONTROL) = TCS_AMC_ENABLE | TCS_AMC_TRIGGER;
		for (unsigned i = 0; i < 2000; i++) {
			if (*(volatile uint32_t *)(tcs + TCS_IRQ_STATUS) & 1) {
				done = true;
				break;
			}
			delay_for_interval(100, NSEC_PER_USEC);
		}
		if (!done) {
			tell("cpufreq: vayu: ddr: RPMh: no answer to the vote\n");
		}
	}
	for (unsigned i = 0; i < count; i++) {
		status[i] = *(volatile uint32_t *)(tcs + TCS_CMD_STATUS(i));
	}
	*(volatile uint32_t *)(tcs + TCS_CONTROL) &= ~TCS_AMC_TRIGGER;
	*(volatile uint32_t *)(tcs + TCS_CONTROL) &= ~TCS_AMC_ENABLE;
	*(volatile uint32_t *)(tcs + TCS_CMD_ENABLE) = 0;
	*(volatile uint32_t *)(tcs + TCS_WAIT_FOR_CMPL) = 0;
	*(volatile uint32_t *)(tcs + TCS_IRQ_CLEAR) = 1;
	*(volatile uint32_t *)(tcs + TCS_IRQ_ENABLE) = irq_enable;
	return done;
}

static void
vote_bandwidth(bool dry)
{
	uint64_t db_phys = VAYU_CMD_DB_PHYS;
	const struct cmd_db_header *db;
	uint32_t mc0, sh0, acv, vote = 0x3fff;
	uint32_t address[3], data[3], status[3] = { 0 };
	bool done, held;

	if (PE_parse_boot_argn("vayu_cmd_db", &db_phys, sizeof(db_phys)) && db_phys == 0) {
		db_phys = VAYU_CMD_DB_PHYS;
	}
	db = (const struct cmd_db_header *)ml_io_map_wcomb((vm_offset_t)db_phys, VAYU_CMD_DB_SIZE);
	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("cpufreq: vayu: ddr: no command database at %#llx (magic %02x %02x %02x %02x)\n", db_phys, db->magic[0],
		    db->magic[1], db->magic[2], db->magic[3]);
		return;
	}
	mc0 = cmd_db_address(db, "MC0");
	sh0 = cmd_db_address(db, "SH0");
	acv = cmd_db_address(db, "ACV");
	tell("cpufreq: vayu: ddr: MC0 %#x SH0 %#x ACV %#x (cmd-db at %#llx)\n", mc0, sh0, acv, db_phys);
	if (mc0 == 0 || sh0 == 0 || acv == 0) {
		tell("cpufreq: vayu: ddr: no vote: a BCM is missing\n");
		return;
	}
	PE_parse_boot_argn("vayu_ddr_vote", &vote, sizeof(vote));
	if (vote == 0 || vote > 0x3fff) {
		tell("cpufreq: vayu: ddr: vote %u is not in 1..0x3fff, not voting\n", vote);
		return;
	}
	if (dry) {
		tell("cpufreq: vayu: ddr: read-only: would vote %u on MC0/SH0 + ACV 0x8\n", vote);
		return;
	}
	address[0] = mc0;
	address[1] = sh0;
	address[2] = acv;
	data[0] = BCM_VOTE(vote, vote);
	data[1] = BCM_VOTE(vote, vote);
	data[2] = BCM_VOTE(0, BCM_ACV_APPS);
	held = vayu_rpmh_tcs_lock();            /* (vayu_pas.c: TCS 0 is shared) */
	done = rpmh_vote(address, data, 3, status);
	vayu_rpmh_tcs_unlock(held);
	tell("cpufreq: vayu: ddr: bandwidth vote %u on MC0/SH0 + ACV 0x8: %s\n", vote, done ? "done" : "NO ANSWER");
	tell("cpufreq: vayu: ddr: CMD_STATUS %#x %#x %#x\n", status[0], status[1], status[2]);
}

/*
 * vayu_thermal (default 1 with cpufreq=1 cpufreq_vayu=1; vayu_thermal=0: off): the temperature,
 * read only, and a throttle for the rows raised above. Every 2 s:
 *   TSENS  two controllers (vayu.dtb tsens@c222000 / tsens@c223000, "qcom,tsens24xx": TM at
 *          0x0c263000 and 0x0c265000, SROT at 0x0c222000 and 0x0c223000). Per sensor n, TM +
 *          0xa0 + 4n: bits [11:0] the last temperature, signed, in tenths of a degree; bit 21 valid.
 *          SROT + 0x04 bits [18:3] which sensors are enabled (mainline tsens-v2.c:15,44,90-92,128-129;
 *          the deci-degree format: tsens.c tsens_hw_to_mC). Sensor names: mainline sm8150.dtsi
 *          thermal-zones (tsens0: 0 aoss0, 1-4 cpu0-3, 5/6 cluster0/1, 7-10 cpu4-7 top, 11-14 cpu4-7
 *          bottom, 15 gpu top; tsens1: 0 aoss1, 1 wlan, 2 video, 3 mem, 4 q6-hvx, 5 camera, 6 compute,
 *          7 modem, 8 npu, 9/10 modem vec/scl, 11 gpu bottom).
 *   PMIC   the temperature alarm at 0x2400 of the PM8150 (slave 0), PM8150B (2) and PM8150L (4)
 *          (vayu.dtb qcom,temp-alarm@2400, "qcom,spmi-temp-alarm"), through hakios_spmi_peek()
 *          (buttons=1): TYPE 0x04 = 0x09, SUBTYPE 0x05 (0x08 gen1, 0x09 gen2), STATUS 0x08 -- gen1
 *          the stage in [1:0], gen2 a state in [6:4] mapped {0,1,1,2,2,3,3,3} (mainline
 *          qcom-spmi-temp-alarm.c:23-43,153). Stage 1 is 90..105 C on the PMIC's die.
 * The throttle: level 0 = the top rows raised above; at vayu_thermal_hot (default 85 C, the
 * hottest valid TSENS sensor) level 1 = a ~3/4 row per raised group; at hot + 10 C, or any PMIC
 * stage >= 1, level 2 = the slowest row and L3 back to the firmware's row. One level down only
 * below the level's own threshold minus 10 C. No valid sensor at all: level 1 (not the top rows
 * blind). Only groups this driver raised are ever written (none with cpufreq_dry=1). Changes go to
 * the loader log at once, a summary every 60 s.
 */
#define TSENS_CONTROLLERS       2
#define TSENS_SENSORS           16
#define TSENS_TM_STATUS(n)      (0x0a0 + 4 * (n))
#define TSENS_SROT_CTRL         0x004
#define TSENS_VALID             (1u << 21)
#define THERMAL_POLL_MS         2000
#define THERMAL_SUMMARY_POLLS   30

static const uint32_t tsens_tm_phys[TSENS_CONTROLLERS] = { 0x0c263000, 0x0c265000 };
static const uint32_t tsens_srot_phys[TSENS_CONTROLLERS] = { 0x0c222000, 0x0c223000 };
static const char *const tsens_name[TSENS_CONTROLLERS][TSENS_SENSORS] = {
	{ "aoss0", "cpu0", "cpu1", "cpu2", "cpu3", "cluster0", "cluster1", "cpu4-top", "cpu5-top", "cpu6-top",
	  "cpu7-top", "cpu4-bottom", "cpu5-bottom", "cpu6-bottom", "cpu7-bottom", "gpu-top" },
	{ "aoss1", "wlan", "video", "mem", "q6-hvx", "camera", "compute", "modem", "npu", "modem-vec", "modem-scl",
	  "gpu-bottom", "?", "?", "?", "?" },
};
static const uint16_t pmic_alarm_id[3] = { 0x024, 0x224, 0x424 };      /* slave << 8 | peripheral 0x24 */
static const char *const pmic_name[3] = { "PM8150", "PM8150B", "PM8150L" };

static struct {
	vm_offset_t tm[TSENS_CONTROLLERS];
	uint32_t enabled[TSENS_CONTROLLERS];    /* sensor mask from SROT */
	int hot;                                /* deci-C */
	uint32_t level;                         /* what the temperature asks for */
	uint32_t applied;                       /* what is written: the higher of level and the SDP hold's */
	bool sdp_hold;                          /* charging from a PC port (SDP, 500 mA) with the battery low: level 2 */
	uint32_t sdp_on, sdp_below;             /* vayu_sdp_slow (default 1), vayu_sdp_below (default 50 %) */
	volatile bool stopped;                  /* set by vayu_cpufreq_quiesce(): no more writes */
} thermal;

/* The hottest valid sensor, in tenths of a degree; false if none is valid. */
static bool
tsens_hottest(int *temp, uint32_t *which, uint32_t *valid)
{
	bool any = false;

	*valid = 0;
	for (uint32_t c = 0; c < TSENS_CONTROLLERS; c++) {
		for (uint32_t n = 0; n < TSENS_SENSORS; n++) {
			uint32_t status;
			int t;

			if (!(thermal.enabled[c] & (1u << n))) {
				continue;
			}
			status = *(volatile uint32_t *)(thermal.tm[c] + TSENS_TM_STATUS(n));
			t = (int)((status & 0xfff) ^ 0x800) - 0x800;
			if (!(status & TSENS_VALID) || t < -400 || t > 1500) {
				continue;
			}
			(*valid)++;
			if (!any || t > *temp) {
				*temp = t;
				*which = c << 8 | n;
				any = true;
			}
		}
	}
	return any;
}

/* The highest stage of the three PMIC alarms; -1 where one cannot be read (the arbiter not mapped yet). */
static int
pmic_stage(int stage[3])
{
	static const uint8_t gen2_map[8] = { 0, 1, 1, 2, 2, 3, 3, 3 };
	int most = -1;

	for (int i = 0; i < 3; i++) {
		uint8_t type, subtype, status;

		stage[i] = -1;
		if (!hakios_spmi_peek(pmic_alarm_id[i], 0x04, &type) || !hakios_spmi_peek(pmic_alarm_id[i], 0x05, &subtype) ||
		    !hakios_spmi_peek(pmic_alarm_id[i], 0x08, &status) || type != 0x09) {
			continue;
		}
		if (subtype == 0x08) {
			stage[i] = status & 0x3;
		} else if (subtype == 0x09) {
			stage[i] = gen2_map[(status >> 4) & 0x7];
		} else {
			continue;
		}
		if (stage[i] > most) {
			most = stage[i];
		}
	}
	return most;
}

static void
thermal_apply(uint32_t level)
{
	for (uint32_t domain = 0; domain < OSM_DOMAINS && !thermal.stopped; domain++) {
		uint32_t row, state;

		if (!raised[domain].wrote) {
			continue;
		}
		row = raised[domain].row[level];
		*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE) = row;
		if (raised[domain].per_core) {
			for (uint32_t core = 1; core < osm_cores[domain]; core++) {
				*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE + 4 * core) = row;
			}
		}
		delay_for_interval(20, NSEC_PER_MSEC);
		__asm__ volatile ("dsb sy" ::: "memory");
		state = *(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE);
		tell("thermal: vayu: group %u -> row %u, %u MHz%s\n", domain, row, raised[domain].khz[level] / 1000,
		    state == row ? "" : " (the hardware reports another row)");
	}
	if (l3_raised.wrote && !thermal.stopped) {
		uint32_t row = level == 2 ? l3_raised.was : l3_raised.top;

		*(volatile uint32_t *)(l3_raised.base + OSM_PERF_STATE) = row;
		tell("thermal: vayu: L3 -> row %u\n", row);
	}
}

/*
 * The SDP hold (maintainers, 2026-10-25: a PC's USB port gives 500 mA, the top rows draw ~800 mA more than that and
 * the battery fell from 31 % to 18 % in a night). Read only, through hakios_spmi_peek() (buttons=1), the PM8150B's:
 * USBIN_INT_RT_STS 0x1310 bit 4 = a cable is in, APSD_RESULT_STATUS 0x1308 bit 0 = SDP (vendor smb5-reg.h:218-227,
 * 239), the monotonic SOC 0x4009 and its shadow 0x400a, percent = raw x 100 / 255 (hakios.battery.c). Held -- the
 * slowest rows (level 2) -- while plugged into an SDP with the battery under vayu_sdp_below %; let go at 5 % more,
 * or when the cable comes out or the port is not an SDP. vayu_sdp_slow=0: never.
 */
static void
sdp_check(void)
{
	uint8_t usbin, apsd, soc[2];
	bool plugged, sdp, held = thermal.sdp_hold;
	uint32_t percent;

	if (!thermal.sdp_on || !hakios_spmi_peek(0x213, 0x10, &usbin) || !hakios_spmi_peek(0x213, 0x08, &apsd) ||
	    !hakios_spmi_peek(0x240, 0x09, &soc[0]) || !hakios_spmi_peek(0x240, 0x0a, &soc[1]) || soc[0] != soc[1]) {
		return;                                 /* (not read this time: as it was) */
	}
	plugged = (usbin & 0x10) != 0;
	sdp = (apsd & 0x01) != 0;
	percent = (soc[0] * 100u + 127) / 255;
	if (!held && plugged && sdp && percent < thermal.sdp_below) {
		thermal.sdp_hold = true;
	} else if (held && (!plugged || !sdp || percent >= thermal.sdp_below + 5)) {
		thermal.sdp_hold = false;
	}
	if (thermal.sdp_hold != held) {
		tell("thermal: vayu: SDP hold %s (cable %s, APSD %#x%s, battery %u %%; below %u %% holds)\n", thermal.sdp_hold ? "ON: the "
		    "slowest rows while a 500 mA PC port charges" : "off", plugged ? "in" : "out", apsd, sdp ? " = SDP" : "", percent,
		    thermal.sdp_below);
	}
}

static void
thermal_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t polls = 0, which = 0, valid = 0;
	int last_pmic = -2;

	for (;;) {
		int temp = 0, stage[3], pmic, threshold;
		bool have = tsens_hottest(&temp, &which, &valid);
		uint32_t want = thermal.level;

		if (thermal.stopped) {
			return;
		}
		pmic = pmic_stage(stage);
		if (!have) {
			want = 1;
		} else {
			if (temp >= thermal.hot + 100) {
				want = 2;
			} else if (temp >= thermal.hot && want < 1) {
				want = 1;
			}
			threshold = want == 2 ? thermal.hot + 100 : thermal.hot;
			if (want > 0 && want == thermal.level && temp < threshold - 100) {
				want--;
			}
		}
		if (pmic >= 1) {
			want = 2;
		}
		if (pmic != last_pmic) {
			tell("thermal: vayu: PMIC alarm stages %s %d, %s %d, %s %d (-1: not read)\n", pmic_name[0], stage[0],
			    pmic_name[1], stage[1], pmic_name[2], stage[2]);
			last_pmic = pmic;
		}
		if (want != thermal.level) {
			if (have) {
				tell("thermal: vayu: hottest %s (tsens%u s%u) %d.%d C, PMIC stage %d: level %u -> %u\n",
				    tsens_name[which >> 8][which & 0xff], which >> 8, which & 0xff, temp / 10, (temp < 0 ? -temp : temp) % 10,
				    pmic, thermal.level, want);
			} else {
				tell("thermal: vayu: no valid TSENS sensor, PMIC stage %d: level %u -> %u\n", pmic, thermal.level, want);
			}
			thermal.level = want;
		} else if (polls % THERMAL_SUMMARY_POLLS == 0) {
			if (have) {
				tell("thermal: vayu: hottest %s (tsens%u s%u) %d.%d C of %u valid, PMIC stage %d, level %u\n",
				    tsens_name[which >> 8][which & 0xff], which >> 8, which & 0xff, temp / 10, (temp < 0 ? -temp : temp) % 10,
				    valid, pmic, thermal.level);
			} else {
				tell("thermal: vayu: no valid TSENS sensor, PMIC stage %d, level %u\n", pmic, thermal.level);
			}
		}
		sdp_check();
		{
			uint32_t target = thermal.sdp_hold && thermal.level < 2 ? 2 : thermal.level;

			if (target != thermal.applied) {
				tell("thermal: vayu: rows to level %u (temperature %u, SDP hold %s)\n", target, thermal.level,
				    thermal.sdp_hold ? "on" : "off");
				thermal.applied = target;
				thermal_apply(target);
			}
		}
		polls++;
		delay_for_interval(THERMAL_POLL_MS, NSEC_PER_MSEC);
	}
}

static void
vayu_thermal_init(void)
{
	uint32_t on = 1, hot = 85;
	thread_t thread;

	PE_parse_boot_argn("vayu_thermal", &on, sizeof(on));
	PE_parse_boot_argn("vayu_thermal_hot", &hot, sizeof(hot));
	thermal.sdp_on = 1;
	thermal.sdp_below = 50;
	PE_parse_boot_argn("vayu_sdp_slow", &thermal.sdp_on, sizeof(thermal.sdp_on));
	PE_parse_boot_argn("vayu_sdp_below", &thermal.sdp_below, sizeof(thermal.sdp_below));
	thermal.sdp_below = thermal.sdp_below > 95 ? 95 : thermal.sdp_below;
	if (on == 0) {
		tell("thermal: vayu: off (vayu_thermal=0)\n");
		return;
	}
	thermal.hot = (int)(hot < 40 ? 40 : hot > 110 ? 110 : hot) * 10;
	for (uint32_t c = 0; c < TSENS_CONTROLLERS; c++) {
		vm_offset_t srot = ml_io_map(tsens_srot_phys[c], 0x1000);
		uint32_t ctrl = *(volatile uint32_t *)(srot + TSENS_SROT_CTRL);

		thermal.tm[c] = ml_io_map(tsens_tm_phys[c], 0x1000);
		thermal.enabled[c] = (ctrl >> 3) & 0xffff;
		tell("thermal: vayu: tsens%u SROT ctrl 0x%08x (%s, sensors 0x%04x)\n", c, ctrl, (ctrl & 1) ? "enabled" : "NOT enabled",
		    thermal.enabled[c]);
		if (!(ctrl & 1)) {
			thermal.enabled[c] = 0;
		}
		for (uint32_t n = 0; n < TSENS_SENSORS; n++) {
			uint32_t status = *(volatile uint32_t *)(thermal.tm[c] + TSENS_TM_STATUS(n));
			int t = (int)((status & 0xfff) ^ 0x800) - 0x800;

			if (thermal.enabled[c] & (1u << n)) {
				tell("thermal: vayu:   tsens%u s%-2u %-12s %d.%d C%s\n", c, n, tsens_name[c][n], t / 10, (t < 0 ? -t : t) % 10,
				    (status & TSENS_VALID) ? "" : " (not valid)");
			}
		}
	}
	tell("thermal: vayu: throttle at %d C (level 1), %d C (level 2), each 10 C lower to step back; poll %u ms\n",
	    thermal.hot / 10, thermal.hot / 10 + 10, THERMAL_POLL_MS);
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		if (raised[domain].wrote) {
			tell("thermal: vayu: group %u levels: row %u %u MHz / row %u %u MHz / row %u %u MHz\n", domain,
			    raised[domain].row[0], raised[domain].khz[0] / 1000, raised[domain].row[1], raised[domain].khz[1] / 1000,
			    raised[domain].row[2], raised[domain].khz[2] / 1000);
		}
	}
	if (kernel_thread_start(thermal_thread, NULL, &thread) != KERN_SUCCESS) {
		tell("thermal: vayu: cannot start the thread\n");
		return;
	}
	thread_deallocate(thread);
}

static bool
cpufreq_wanted(void)
{
	uint32_t on = 0, vayu = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0 &&
	       PE_parse_boot_argn("cpufreq_vayu", &vayu, sizeof(vayu)) && vayu != 0;
}

void
vayu_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t cores[OSM_DOMAINS] = { 0 }, dry = 0, force = 0, turbo = 0, l3 = 1, ddr = 0, here, before;

	if (!cpufreq_wanted()) {
		return;
	}
	PE_parse_boot_argn("cpufreq_dry", &dry, sizeof(dry));
	PE_parse_boot_argn("cpufreq_force", &force, sizeof(force));
	PE_parse_boot_argn("cpufreq_turbo", &turbo, sizeof(turbo));
	PE_parse_boot_argn("cpufreq_l3", &l3, sizeof(l3));
	PE_parse_boot_argn("vayu_ddr", &ddr, sizeof(ddr));
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	tell("cpufreq: vayu: %u of our cores in group 0, %u in group 1, %u in group 2%s\n", cores[0], cores[1], cores[2],
	    dry ? "; read-only (cpufreq_dry=1)" : "");
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain], dry != 0, force != 0, turbo != 0);
		}
	}
	if (l3 != 0) {
		raise_l3(dry != 0);
	}
	if (ddr != 0) {
		vote_bandwidth(dry != 0);
	}
	if (!dry) {
		tell("cpufreq: vayu: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	}
	vayu_thermal_init();
}

/*
 * On the way back to the loader (osfmk/arm64/loader_return.c, whatever the reason): the row the
 * firmware had left, so that the loader and the next boot do not run at the top row for nothing.
 * (The registers are volatile; a reset gives the firmware's own state back anyway.)
 *
 * The memory bandwidth vote is not taken back here: the firmware's own vote cannot be read back,
 * and a maximum vote is only power.
 */
void
vayu_cpufreq_quiesce(void)
{
	thermal.stopped = true;
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		if (!raised[domain].wrote) {
			continue;
		}
		raised[domain].wrote = false;
		*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE) = raised[domain].was;
		if (raised[domain].per_core) {
			for (uint32_t core = 1; core < osm_cores[domain]; core++) {
				*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE + 4 * core) = raised[domain].was;
			}
		}
		tell("cpufreq: vayu: group %u back to row %u for the loader\n", domain, raised[domain].was);
	}
	if (l3_raised.wrote) {
		l3_raised.wrote = false;
		*(volatile uint32_t *)(l3_raised.base + OSM_PERF_STATE) = l3_raised.was;
		tell("cpufreq: vayu: L3 back to row %u for the loader\n", l3_raised.was);
	}
}
