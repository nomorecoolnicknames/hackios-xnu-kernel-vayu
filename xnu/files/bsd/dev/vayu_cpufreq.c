/*
 * The clock speed of the processor cores the kernel runs on, on the POCO X3 Pro (vayu, SM8150-AC).
 * Only with boot-args cpufreq=1 cpufreq_vayu=1: the addresses are this phone's. Without them,
 * vayu_thermal_monitor=1 (default 0) runs the temperature throttle alone: nothing is raised, no
 * governor, no SDP hold, L3 and DDR left alone; it only reads while cold, and when hot it steps the
 * groups down from the rows the firmware left (never above them).
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
 * With cpufreq_gov=1 (the default now; cpufreq_gov=0: nothing changes) the row of each group follows
 * the load: the idle time of every core is read every cpufreq_gov_ms and a quiet group goes to the
 * slowest row, a busy one to the top (see the "governor" section below). With cpufreq_gov=0 the top
 * row stays written from boot until the temperature throttle (vayu_thermal, 75 C) takes a step down.
 *
 * Power (o6, all off by default; each one changes nothing unless it is given):
 *   cpufreq_gov_prop=1     the governor picks any ordinary row of the table in proportion to the load of
 *                          the busiest core of the group, instead of the three rows top / ~3/4 / slowest
 *                          (see "the proportional governor" below); cpufreq_gov_head=N (125), _up=N (1),
 *                          _down=N (3), _dwell_ms=N (300) tune it. The thermal and SDP levels still cap it.
 *   cpufreq_touch_boost_pct=N  (20..100, default 100 = the top row, as before) what the touch boost asks
 *                          for with cpufreq_gov_prop=1: the row at no more than N % of the top.
 *   cpufreq_touch_boost_fix=1  the boost's hold (cpufreq_touch_boost_ms, 300) in real milliseconds. Without
 *                          it the touch's idle time (19.2 MHz ticks) is compared with nanoseconds, and every
 *                          touch keeps all groups on their top rows for 15.6 s.
 *   cpufreq_screen_off=1   while iOS has the display off (RequestPowerChange 0, MiniIOMobileFramebuffer),
 *                          after cpufreq_screen_off_ms (5000), every group at its slowest row, as the SDP
 *                          hold does; the display back on lets go at once.
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
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/vayu_cpufreq.h>
#include <sys/vayu_pas.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern bool hakios_spmi_peek(uint16_t id, uint8_t reg, uint8_t *value);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);        /* osfmk/arm64/machine_routines.c */

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
	uint32_t written;       /* the row thermal_apply() last wrote (UINT32_MAX: none) */
	bool per_core;
	bool wrote;             /* a row of ours is in the hardware: vayu_cpufreq_quiesce() puts `was' back */
	bool managed;           /* the throttle steps this group: raised, or left at its top by the firmware */
	uint32_t steps;                         /* cpufreq_gov_prop: the ladder below, fastest first */
	uint32_t ladder_row[OSM_ROWS];
	uint32_t ladder_khz[OSM_ROWS];
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

/*
 * cpufreq_gov_prop: every ordinary row of the table, one per frequency, fastest first, and none faster
 * than the top row found above (heat3's ladder, k32-heat; run d-1081c7 idled at its slowest steps).
 * The table's order is not assumed. A table with no usable row leaves steps at 0: the proportional
 * governor then leaves the group to the three rows above.
 */
static void
build_ladder(vm_offset_t base, uint32_t rows, bool turbo, const struct osm_row *top, struct raised_domain *d)
{
	uint32_t n = 0;

	for (uint32_t i = 0; i < rows && i < OSM_ROWS; i++) {
		struct osm_row r;
		uint32_t at;

		read_row(base, i, &r);
		if ((r.cores == OSM_SINGLE_CORE && !turbo) || r.khz > top->khz) {
			continue;
		}
		for (at = 0; at < n && d->ladder_khz[at] > r.khz; at++) {
		}
		if (at < n && d->ladder_khz[at] == r.khz) {
			continue;               /* (the table repeats frequencies: one step is enough) */
		}
		for (uint32_t j = n; j > at; j--) {
			d->ladder_khz[j] = d->ladder_khz[j - 1];
			d->ladder_row[j] = d->ladder_row[j - 1];
		}
		d->ladder_khz[at] = r.khz;
		d->ladder_row[at] = r.index;
		n++;
	}
	d->steps = n != 0 && d->ladder_khz[0] == top->khz ? n : 0;
}

static void
raise_domain(uint32_t domain, uint32_t cores, bool dry, bool force, bool turbo, bool monitor)
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
	if (monitor && !have_now) {
		return;
	}
	if (have_now && (monitor || top.khz <= now.khz)) {
		/*
		 * Not raised -- the firmware's row is as fast as ours, or only the monitor runs -- but still the
		 * throttle's: a group the loader left at its top row was never slowed down when hot (review R1.2).
		 * Its level 0 is the firmware's row; nothing is written until a level asks for another.
		 */
		if (!dry) {
			raised[domain].base = base;
			raised[domain].was = state;
			raised[domain].per_core = (dcvs & 1) != 0;
			thermal_rows(base, rows, turbo, &now, raised[domain].row, raised[domain].khz);
			raised[domain].written = state;
			raised[domain].managed = true;
		}
		tell("cpufreq: vayu: group %u: keeping the firmware's row %u (%u MHz)%s\n", domain, state, now.khz / 1000,
		    dry ? "; read-only" : "; the thermal throttle steps down from it");
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
	build_ladder(base, rows, turbo, &top, &raised[domain]);
	raised[domain].written = top.index;
	raised[domain].wrote = true;
	raised[domain].managed = true;
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
	uint32_t rest;                          /* level 2's row: the firmware's, or the slowest if that was the top */
	uint32_t written;                       /* the row thermal_apply() last wrote */
	bool wrote, managed;
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
		/* (left at its top by the firmware: the throttle's all the same, its level 2 the slowest row) */
		if (!dry) {
			uint32_t row[3], khz[3];

			thermal_rows(base, rows, true, &now, row, khz);
			l3_raised.base = base;
			l3_raised.was = l3_raised.top = l3_raised.written = state;
			l3_raised.rest = row[2];
			l3_raised.managed = true;
		}
		tell("cpufreq: vayu: L3: keeping the firmware's row %u (%u MHz)%s\n", state, now.khz / 1000,
		    dry ? "; read-only" : "; the thermal throttle steps down from it");
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
	l3_raised.was = l3_raised.rest = state;
	l3_raised.top = top.index;
	l3_raised.written = top.index;
	l3_raised.wrote = true;
	l3_raised.managed = true;
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

/* One wait that does not sleep: the TCS lock is held by the caller, and a sleeper would sit on the
 * apps RSC's one command slot across a scheduler tick (vayu_pas.c's vayu_rsc_tcs_lock()). */
static void
rpmh_spin_us(uint32_t us)
{
	uint64_t deadline;

	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	while (mach_absolute_time() < deadline) {
	}
}

/* One active-only request in command slot set 0, and the wait until RPMh has done it (as
 * vayu_qup_spi.c). The slot's mapping is the caller's, taken before the lock: mapping may sleep. */
static bool
rpmh_vote(vm_offset_t tcs, const uint32_t *address, const uint32_t *data, unsigned count, uint32_t *status)
{
	uint32_t irq_enable;
	bool idle = false, done = false;

	irq_enable = *(volatile uint32_t *)(tcs + TCS_IRQ_ENABLE);
	tell("cpufreq: vayu: ddr: tcs0 status %#x control %#x irq en %#x st %#x cmd_en %#x\n",
	    *(volatile uint32_t *)(tcs + TCS_STATUS), *(volatile uint32_t *)(tcs + TCS_CONTROL), irq_enable,
	    *(volatile uint32_t *)(tcs + TCS_IRQ_STATUS), *(volatile uint32_t *)(tcs + TCS_CMD_ENABLE));
	for (unsigned i = 0; i < 2000; i++) {
		if (*(volatile uint32_t *)(tcs + TCS_STATUS) != 0) {
			idle = true;
			break;
		}
		rpmh_spin_us(100);
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
			rpmh_spin_us(100);
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
	vm_offset_t tcs;
	bool done;
	enum vayu_rsc_tcs_result held;

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
	tcs = ml_io_map(VAYU_RSC_TCS_PHYS, VAYU_RSC_TCS_SIZE);   /* (the mapping may sleep: not under the lock) */
	if (tcs == 0) {
		tell("cpufreq: vayu: ddr: cannot map the RSC\n");
		return;
	}
	held = vayu_rsc_tcs_lock("cpufreq");    /* (vayu_pas.c: THE one lock of TCS 0) */
	if (held == VAYU_RSC_TCS_FAILED) {
		tell("cpufreq: vayu: ddr: TCS 0 was not taken: not voting\n");
		return;
	}
	done = rpmh_vote(tcs, address, data, 3, status);
	if (held == VAYU_RSC_TCS_HELD) {
		vayu_rsc_tcs_unlock();
	}
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
 * The throttle: level 0 = the top rows raised above; at vayu_thermal_hot (default 75 C, the
 * hottest valid TSENS sensor) level 1 = a ~3/4 row per raised group; at hot + 10 C, or any PMIC
 * stage >= 1, level 2 = the slowest row and L3 back to the firmware's row. One level down only
 * below the level's own threshold minus 10 C, and (level 1 -> 0) only after the level has held for
 * vayu_thermal_hold_ms (default 3000): the junction swings ~20 C a second under a burst, so without
 * the hold the throttle alone makes a 68..91 C sawtooth (see the note above thermal_apply()).
 * No valid sensor at all: level 2 at least, and a level already in force is never relaxed (a hot
 * core whose sensors stopped answering is not sped up); a PMIC alarm that cannot be read keeps the
 * level from going down. Only groups this driver raised, or found at their top row, are ever
 * written (none with cpufreq_dry=1), and only rows that changed. Changes go to the loader log at
 * once, a summary every 60 s.
 */
#define TSENS_CONTROLLERS       2
#define TSENS_SENSORS           16
#define TSENS_TM_STATUS(n)      (0x0a0 + 4 * (n))
#define TSENS_SROT_CTRL         0x004
#define TSENS_VALID             (1u << 21)
#define THERMAL_POLL_MS         2000    /* the PMIC alarm and the SDP check: register reads over SPMI */
#define THERMAL_TICK_MS         100     /* the TSENS sensors: 32 words of memory, read every tick */

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
	int temp;                               /* the last hottest sensor, deci-C (thermal.have: valid) */
	bool have;
	uint64_t level_since;                   /* when thermal.level last went up (the hold below) */
	uint32_t hold_ms;                       /* vayu_thermal_hold_ms: no step back 1 -> 0 before this */
	uint32_t applied;                       /* signature of what is written (see applied_signature()) */
	bool sensors;                           /* vayu_thermal: the TSENS/PMIC side is on */
	bool sdp_hold;                          /* charging from a PC port (SDP, 500 mA) with the battery low: level 2 */
	uint32_t sdp_on, sdp_below;             /* vayu_sdp_slow (default 1), vayu_sdp_below (default 50 %) */
	volatile bool stopped;                  /* set by vayu_cpufreq_quiesce(): no more writes */
} thermal;

/*
 * cpufreq_gov=1 (the default; cpufreq_gov=0: nothing changes): a load governor on the rows the
 * throttle already steps through. Every cpufreq_gov_ms (default 100, 20..1000) the idle time of
 * every core of ours (ml_cpu_idle_time(), as hakios.cpufreq.c's cpuload does) is read, and each OSM
 * group takes
 *   level 0  the top row, while the group's cores were 50 % busy or more;
 *   level 1  the ~3/4 row, between 15 and 50 %;
 *   level 2  the slowest row, under 15 % -- after three samples in a row (300 ms), so that the
 *            bursts of the framebuffer's copy and of a gesture do not throw it down and up again.
 * The two thresholds have 5 points of hysteresis on the way back down (45 and 10 %): a group whose
 * load sits on a threshold -- cpu7 alone, 11..17 % in the 03.11 log -- otherwise flips its row
 * every 300 ms, and every flip used to write all three groups and L3 and print five lines.
 * A busier group goes back up at once. The governor only ever asks for a slower row than the
 * temperature/SDP level does: thermal_apply() takes the higher of the two. It writes only groups
 * the driver raised, and nothing after vayu_cpufreq_quiesce().
 *
 * The log: quiet by default. One summary line every cpufreq_gov_sum_ms (default 10000); a line per
 * row change only with cpufreq_gov_log=1, and then no more than once per group per
 * cpufreq_gov_log_ms (default 5000).
 */
#define GOVERNOR_BUSY_TOP       50      /* per cent of the cores' time, per group */
#define GOVERNOR_BUSY_MID       15
#define GOVERNOR_BACK            5      /* points below a threshold before a group leaves its level */
#define GOVERNOR_QUIET_SAMPLES  3

/*
 * The proportional governor (cpufreq_gov_prop=1; o6). The three rows above put a group on its top row
 * from 50 % busy: run 1078 (03.11, k30, screen off from 223 s on) kept the big cores at 1804-2419 MHz
 * and the prime core at 2956 MHz at 25-45 % and 11-100 % busy, 68-80 C, the battery giving 350-575 mA
 * on top of the USB port's 500. Here a group takes the slowest step of build_ladder()'s whose frequency
 * covers
 *
 *   the frequency now x the busy share of the group's busiest core x cpufreq_gov_head (125 %)
 *
 * -- the busiest core and not the mean, because the group's cores share one clock: one thread that
 * keeps one big core busy needs the clock that core needs, however idle its two neighbours are (the
 * mean of the three asked for a third of it). The frequency now and not the top: at a slower row the
 * same work is a larger share, and asking from the top would never settle (90 % or more: twice the
 * frequency now, see governor_prop_sample()). The share rises at once and
 * falls smoothed (7/10 of the old); a step up is taken after cpufreq_gov_up samples (1), at once when it
 * is two steps or more; a step down after cpufreq_gov_down (3) and no sooner than cpufreq_gov_dwell_ms
 * (300) after the group's last change. The thermal and SDP levels stay caps: the slower of the two rows
 * is written (wanted_row()). A row change is written without the 20 ms wait and the line, which are for
 * cpufreq_gov_log=1: Linux's qcom-cpufreq-hw writes the same register and does not wait either.
 */
#define GOVERNOR_SMOOTH_OLD     7       /* tenths of the old share kept on the way down */
#define GOVERNOR_SATURATED      90      /* per cent: the busiest core had no idle time to speak of */

/*
 * cpufreq_touch_boost=1 (default 1; 0: off): while a finger is on the screen, and for
 * cpufreq_touch_boost_ms (default 300, 0..5000) after the last sample, every group asks for its top
 * row regardless of the load. The load governor's thresholds are averages over cpufreq_gov_ms and a
 * quiet group needs GOVERNOR_QUIET_SAMPLES samples before it drops: at the moment a finger lands
 * after an idle screen the groups are already at the slowest row (03.11, d12b: 300/710/825 MHz), and
 * the first frames of that scroll were 34-75 ms apart while the window server asked for 120 a second
 * (670.9 s: 20 frames in 1267 ms, 167 touch samples). The temperature still wins: thermal_apply()
 * takes the higher of this and the thermal level, so the boost can never go above the throttle's cap.
 *
 * bsd/dev/hakios.input.c: 0 while a finger is down, otherwise the time since the last sample in
 * mach_absolute_time units (not nanoseconds: see cpufreq_touch_boost_fix), or ~0 when there has never
 * been one.
 */
extern uint64_t hakios_touch_idle_abs(void);

static struct {
	bool on;
	bool log;                               /* cpufreq_gov_log=1: a line per row change (rate limited) */
	uint32_t every_ms;
	uint32_t log_ms;                        /* cpufreq_gov_log_ms: the least gap between two such lines */
	uint32_t sum_ms;                        /* cpufreq_gov_sum_ms: the summary line's period (0: none) */
	uint32_t touch_boost;                   /* cpufreq_touch_boost: the top rows while a finger is down */
	uint32_t touch_boost_ms;                /* cpufreq_touch_boost_ms: how long after the last sample */
	bool boost;                             /* a finger is down (or was within touch_boost_ms): the top rows */
	bool boosted;                           /* the last sample was boosted (for the one line per change) */
	uint32_t level[OSM_DOMAINS];            /* 0 top, 1 the ~3/4 row, 2 the slowest row */
	uint32_t percent[OSM_DOMAINS];          /* the last sample, for the summary */
	uint32_t quiet[OSM_DOMAINS];            /* samples in a row that ask for a slower level */
	uint64_t told[OSM_DOMAINS];             /* when this group's row change was last printed, ns */
	uint64_t idle_before[MAX_CPUS];
	uint64_t then;
	bool prop;                              /* cpufreq_gov_prop=1: the proportional governor */
	uint32_t head, up, down, dwell_ms;      /* cpufreq_gov_head/_up/_down/_dwell_ms */
	uint32_t boost_pct;                     /* cpufreq_touch_boost_pct */
	bool boost_fix;                         /* cpufreq_touch_boost_fix=1: the hold in real milliseconds */
	uint32_t step[OSM_DOMAINS];             /* the ladder step the load asks for: 0 the top row */
	uint32_t share[OSM_DOMAINS];            /* the busiest core's share, x10, smoothed */
	uint32_t pending[OSM_DOMAINS], pending_n[OSM_DOMAINS];  /* the step wanted next, and for how many samples */
	uint64_t changed[OSM_DOMAINS];          /* when the group's step last changed, ns */
	uint32_t screen_off;                    /* cpufreq_screen_off=1 */
	uint32_t screen_off_ms;                 /* cpufreq_screen_off_ms */
	volatile bool display_off;              /* iOS has the display off (vayu_cpufreq_display()) */
	volatile uint64_t display_off_at;       /* ... since then, ns */
	bool screen_hold;                       /* the slowest rows because of it (the thermal thread's view) */
} governor;

/* The row the group should be at: the slower (higher) of the temperature's and the load's. */
static uint32_t
effective_level(uint32_t domain)
{
	uint32_t level = thermal.level;

	if (thermal.sdp_hold || governor.screen_hold) {
		level = 2;
	}
	return governor.level[domain] > level ? governor.level[domain] : level;
}

/* The row to write for a group: its level's, or with cpufreq_gov_prop=1 the governor's step when that
 * one is slower (the levels stay caps; governor.level[] is never raised in that mode). */
static uint32_t
wanted_row(uint32_t domain, uint32_t *khz)
{
	const struct raised_domain *d = &raised[domain];
	uint32_t level = effective_level(domain), row = d->row[level], k = d->khz[level];

	if (governor.on && governor.prop && d->steps != 0) {
		uint32_t step = governor.step[domain] < d->steps ? governor.step[domain] : d->steps - 1;

		if (d->ladder_khz[step] < k) {
			row = d->ladder_row[step];
			k = d->ladder_khz[step];
		}
	}
	if (khz != NULL) {
		*khz = k;
	}
	return row;
}

/* The row each group is at now (the higher of the load governor's and the temperature's) and its
 * frequency in MHz: for the display driver's once-a-second scroll line (scroll120), which says
 * whether a finger's frames were slow because the cores were still on the slowest row. */
void
vayu_cpufreq_rows(uint32_t row[OSM_DOMAINS], uint32_t mhz[OSM_DOMAINS])
{
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		uint32_t level = effective_level(domain), khz;

		if (!raised[domain].managed || level > 2) {
			row[domain] = UINT32_MAX;
			mhz[domain] = 0;
			continue;
		}
		row[domain] = wanted_row(domain, &khz);
		mhz[domain] = khz / 1000;
	}
}

/* What thermal_apply() would write now: 0 is never a valid signature. The governor's own level is
 * not in it: what matters is the row the two of them agree on, so a governor step that does not
 * change that row must not make thermal_apply() write (and wait 20 ms) again. */
static uint32_t
applied_signature(void)
{
	uint32_t signature = (thermal.sdp_hold ? 1 : 0) + 1;

	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		signature = signature * 4 + effective_level(domain);
	}
	if (governor.on && governor.prop) {
		/* (the rows themselves: a step of the proportional governor is not a level) */
		for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
			signature = signature * OSM_ROWS + wanted_row(domain, NULL);
		}
	}
	return signature;
}

/* Nanoseconds on the monotonic clock (the same base as the governor's interval arithmetic). */
static uint64_t
now_ns(void)
{
	uint64_t nanos = 0;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanos);
	return nanos;
}

/*
 * cpufreq_screen_off=1: iOS's display power (MiniIOMobileFramebuffer, RequestPowerChange: in[0] 0 the
 * display off). Only noted here; the thermal thread holds the slowest rows once the display has been off
 * for cpufreq_screen_off_ms and lets go at its next tick after the display is back.
 */
void
vayu_cpufreq_display(bool on)
{
	if (!on && !governor.display_off) {
		governor.display_off_at = now_ns();
	}
	governor.display_off = !on;
}

/* cpufreq_gov_prop: the slowest ladder step whose frequency is at least `khz` (0, the top, if none is). */
static uint32_t
ladder_step_for(const struct raised_domain *d, uint64_t khz)
{
	uint32_t step = 0;

	for (uint32_t i = 1; i < d->steps && d->ladder_khz[i] >= khz; i++) {
		step = i;
	}
	return step;
}

/*
 * One sample of the proportional governor for one group: `most` is the busiest core's share of the
 * interval in per cent. Only governor.step[] changes here; thermal_apply() writes the row.
 */
static void
governor_prop_sample(uint32_t domain, uint32_t most)
{
	const struct raised_domain *d = &raised[domain];
	uint32_t now, want, share;
	uint64_t nanoseconds;

	if (most * 10 > governor.share[domain]) {
		governor.share[domain] = most * 10;
	} else {
		governor.share[domain] = (governor.share[domain] * GOVERNOR_SMOOTH_OLD + most * 10 * (10 - GOVERNOR_SMOOTH_OLD)) / 10;
	}
	share = (governor.share[domain] + 5) / 10;
	governor.percent[domain] = share;
	if (!d->wrote || d->steps < 2) {
		return;
	}
	now = governor.step[domain] < d->steps ? governor.step[domain] : d->steps - 1;
	/*
	 * A core busy the whole interval says only that it needs more, not how much: twice the frequency
	 * then (the slowest row to the top in three samples), not 125 % of it a step at a time.
	 */
	want = ladder_step_for(d, share >= GOVERNOR_SATURATED ? 2ULL * d->ladder_khz[now] :
	    (uint64_t)d->ladder_khz[now] * share / 100 * governor.head / 100);
	if (governor.boost) {
		uint64_t most_khz = (uint64_t)d->ladder_khz[0] * governor.boost_pct / 100;
		uint32_t floor = d->steps - 1;

		/* a finger: at least the fastest step at no more than boost_pct of the top */
		for (uint32_t i = 0; i < d->steps; i++) {
			if (d->ladder_khz[i] <= most_khz) {
				floor = i;
				break;
			}
		}
		if (floor < want) {
			want = floor;
		}
	}
	if (want == now) {
		governor.pending_n[domain] = 0;
		return;
	}
	/*
	 * A run of samples that all ask the same way; the step taken at its end is the least change any of
	 * them asked for (the smoothed share still wobbles by a step from one sample to the next).
	 */
	if (governor.pending_n[domain] == 0 || (governor.pending[domain] < now) != (want < now)) {
		governor.pending[domain] = want;
		governor.pending_n[domain] = 0;
	} else if (want < now ? want > governor.pending[domain] : want < governor.pending[domain]) {
		governor.pending[domain] = want;
	}
	governor.pending_n[domain]++;
	nanoseconds = now_ns();
	if (want < now) {
		/* faster: after cpufreq_gov_up samples, or at once for two steps or more (a burst begins) */
		if (now - want >= 2) {
			governor.pending[domain] = want;
		} else if (governor.pending_n[domain] < governor.up) {
			return;
		}
	} else if (governor.pending_n[domain] < governor.down ||
	    nanoseconds - governor.changed[domain] < (uint64_t)governor.dwell_ms * NSEC_PER_MSEC) {
		return;
	}
	want = governor.pending[domain];
	governor.step[domain] = want;
	governor.changed[domain] = nanoseconds;
	governor.pending_n[domain] = 0;
	if (governor.log && nanoseconds - governor.told[domain] >= (uint64_t)governor.log_ms * NSEC_PER_MSEC) {
		governor.told[domain] = nanoseconds;
		tell("cpufreq: vayu: governor: group %u %u%% busy (busiest core): step %u of %u, row %u, %u MHz\n", domain, share,
		    want, d->steps, d->ladder_row[want], d->ladder_khz[want] / 1000);
	}
}

static void
governor_sample(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t now = mach_absolute_time();
	uint32_t busy[OSM_DOMAINS] = { 0 }, cores[OSM_DOMAINS] = { 0 }, most[OSM_DOMAINS] = { 0 };

	if (governor.then == 0) {
		/* the first sample only takes the counters: there is no interval to divide by yet */
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0;

			if (ml_cpu_idle_time((int)i, &idle)) {
				governor.idle_before[i] = idle;
			}
		}
		governor.then = now;
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		uint64_t idle = 0, idle_then, delta = now - governor.then;
		uint32_t domain;

		if (!ml_cpu_idle_time((int)i, &idle)) {
			continue;
		}
		idle_then = governor.idle_before[i];
		governor.idle_before[i] = idle;
		if (idle < idle_then || delta == 0) {
			continue;
		}
		domain = domain_of(topology->cpus[i].phys_id);
		if (domain >= OSM_DOMAINS) {
			continue;
		}
		cores[domain]++;
		if (delta > idle - idle_then) {
			uint32_t one = (uint32_t)(100 * (delta - (idle - idle_then)) / delta);

			busy[domain] += one;
			if (one > most[domain]) {
				most[domain] = one;
			}
		}
	}
	governor.then = now;
	/*
	 * A finger on the screen asks for the top rows at once (cpufreq_touch_boost): the load numbers
	 * above are an average of the last cpufreq_gov_ms, so they are the *end* of the quiet interval a
	 * scroll begins in, not the work the scroll is about to do. thermal_apply() still takes the higher
	 * of this and the thermal level, so a hot junction is not overruled.
	 */
	{
		bool boost = false;

		if (governor.touch_boost != 0) {
			uint64_t idle = hakios_touch_idle_abs();
			uint64_t hold = (uint64_t)governor.touch_boost_ms * NSEC_PER_MSEC;

			/*
			 * (idle is in mach_absolute_time units, 19.2 MHz here, not nanoseconds: compared with
			 * nanoseconds the 300 ms hold is 15.6 s -- d1081c2: 6 boosts, 163 s of 490 at the top.
			 * cpufreq_touch_boost_fix=1 compares like with like; without it the hold is as it was.)
			 */
			if (governor.boost_fix) {
				nanoseconds_to_absolutetime((uint64_t)governor.touch_boost_ms * NSEC_PER_MSEC, &hold);
			}
			boost = idle == 0 || idle < hold;
		}
		if (boost != governor.boosted) {
			governor.boosted = boost;
			tell("cpufreq: vayu: touch boost %s: the rows go to the top %s\n", boost ? "on" : "off",
			    boost ? "while a finger is down" : "back to the load");
		}
		governor.boost = boost;
	}
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		uint32_t percent = cores[domain] != 0 ? busy[domain] / cores[domain] : 0;
		uint32_t want, level = governor.level[domain];

		if (governor.prop && raised[domain].steps >= 2) {
			governor_prop_sample(domain, most[domain]);
			continue;               /* (a group with no ladder keeps the three rows below) */
		}
		/* (the thresholds differ on the way down: a load that sits on one does not flip the row) */
		if (governor.boost) {
			want = 0;
		} else if (percent >= GOVERNOR_BUSY_TOP) {
			want = 0;
		} else if (level == 0 && percent >= GOVERNOR_BUSY_TOP - GOVERNOR_BACK) {
			want = 0;
		} else if (percent >= GOVERNOR_BUSY_MID) {
			want = 1;
		} else if (level <= 1 && percent >= GOVERNOR_BUSY_MID - GOVERNOR_BACK) {
			want = 1;
		} else {
			want = 2;
		}
		governor.percent[domain] = percent;
		if (want < level) {
			governor.level[domain] = want;          /* busier: at once */
			governor.quiet[domain] = 0;
		} else if (want > level) {
			if (++governor.quiet[domain] >= GOVERNOR_QUIET_SAMPLES) {
				governor.level[domain] = want;  /* quieter for a while: down */
				governor.quiet[domain] = 0;
			}
		} else {
			governor.quiet[domain] = 0;
		}
		if (governor.level[domain] != level && raised[domain].managed && governor.log) {
			uint64_t nanoseconds = now_ns();

			if (nanoseconds - governor.told[domain] >= (uint64_t)governor.log_ms * NSEC_PER_MSEC) {
				governor.told[domain] = nanoseconds;
				tell("cpufreq: vayu: governor: group %u %u%% busy: row %u, %u MHz\n", domain, percent,
				    raised[domain].row[governor.level[domain]], raised[domain].khz[governor.level[domain]] / 1000);
			}
		}
	}
}

/* One line every cpufreq_gov_sum_ms: the rows, the load and the hottest sensor. */
static void
governor_summary(void)
{
	char line[192];

	if (governor.prop) {
		uint32_t khz[OSM_DOMAINS] = { 0 };

		for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
			if (raised[domain].managed) {
				(void)wanted_row(domain, &khz[domain]);
			}
		}
		/* (the busiest core of each group, smoothed; the MHz are what is written, caps included) */
		snprintf(line, sizeof(line), "cpufreq: vayu: governor: steps %u/%u/%u, %u/%u/%u MHz, busiest %u/%u/%u %%, %d.%d C%s",
		    governor.step[0], governor.step[1], governor.step[2], khz[0] / 1000, khz[1] / 1000, khz[2] / 1000,
		    governor.percent[0], governor.percent[1], governor.percent[2], thermal.temp / 10,
		    (thermal.temp < 0 ? -thermal.temp : thermal.temp) % 10, governor.screen_hold ? ", screen off: slowest" : "");
		tell("%s\n", line);
		return;
	}
	if (thermal.have) {
		snprintf(line, sizeof(line), "cpufreq: vayu: governor: rows %u/%u/%u, busy %u/%u/%u %%, %d.%d C",
		    governor.level[0], governor.level[1], governor.level[2], governor.percent[0], governor.percent[1],
		    governor.percent[2], thermal.temp / 10, (thermal.temp < 0 ? -thermal.temp : thermal.temp) % 10);
	} else {
		snprintf(line, sizeof(line), "cpufreq: vayu: governor: rows %u/%u/%u, busy %u/%u/%u %%", governor.level[0],
		    governor.level[1], governor.level[2], governor.percent[0], governor.percent[1], governor.percent[2]);
	}
	tell("%s\n", line);
}

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

/*
 * The rows the levels ask for, and only those that changed. The old version wrote every group on
 * every signature change: with the governor on, one step of one group (cpu7's own group flips on
 * the 15 % line every 300 ms) rewrote all three groups and L3, waiting 20 ms after each write --
 * 63..80 ms of the thermal thread and five log lines, 2780 times in the 03.11 log. Now a group
 * whose row is already the wanted one is left alone: no write, no wait, no line.
 */
static void
thermal_apply(void)
{
	for (uint32_t domain = 0; domain < OSM_DOMAINS && !thermal.stopped; domain++) {
		uint32_t row, state, level, khz;

		if (!raised[domain].managed) {
			continue;
		}
		level = effective_level(domain);
		row = wanted_row(domain, &khz);
		if (row == raised[domain].written) {
			continue;
		}
		raised[domain].wrote = true;
		*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE) = row;
		if (raised[domain].per_core) {
			for (uint32_t core = 1; core < osm_cores[domain]; core++) {
				*(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE + 4 * core) = row;
			}
		}
		raised[domain].written = row;
		if (governor.on && governor.prop && !governor.log) {
			__asm__ volatile ("dsb sy" ::: "memory");
			continue;               /* (cpufreq_gov_prop: no wait, no line; see "the proportional governor") */
		}
		delay_for_interval(20, NSEC_PER_MSEC);
		__asm__ volatile ("dsb sy" ::: "memory");
		state = *(volatile uint32_t *)(raised[domain].base + OSM_PERF_STATE);
		tell("thermal: vayu: group %u -> row %u, %u MHz (level %u)%s\n", domain, row, khz / 1000,
		    level, state == row ? "" : " (the hardware reports another row)");
	}
	if (l3_raised.managed && !thermal.stopped) {
		uint32_t level = 2, any = 0, row;

		/* L3 is shared: it follows the busiest group (the lowest level), and only all-idle drops it */
		for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
			uint32_t use;

			if (!raised[domain].managed) {
				continue;
			}
			use = effective_level(domain);
			if (governor.on && governor.prop && use < 2) {
				uint32_t khz;

				/* (cpufreq_gov_prop: a group in the slower half of its range does not keep L3 fast) */
				(void)wanted_row(domain, &khz);
				if ((uint64_t)khz * 2 <= raised[domain].khz[0]) {
					use = 2;
				}
			}
			if (use < level) {
				level = use;
			}
			any = 1;
		}
		if (!any) {
			level = thermal.level;
		}
		row = level == 2 ? l3_raised.rest : l3_raised.top;
		if (row != l3_raised.written) {
			l3_raised.wrote = true;
			*(volatile uint32_t *)(l3_raised.base + OSM_PERF_STATE) = row;
			l3_raised.written = row;
			tell("thermal: vayu: L3 -> row %u\n", row);
		}
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
	uint32_t tick = THERMAL_TICK_MS;
	uint32_t gov_every = 1, pmic_every, summary_every;
	uint64_t governor_sum_then = 0;
	int last_pmic = -2, pmic = -1;
	uint32_t missing = 0, last_missing = UINT32_MAX;        /* the PMIC alarms not read (bit per PMIC) */

	/* The governor's own period may be shorter than the sensors'; the other way round the loop
	 * still wakes at THERMAL_TICK_MS, so a hot junction is seen 0.1 s after it crossed, not 2 s. */
	if (governor.on && governor.every_ms < tick) {
		tick = governor.every_ms;
	}
	if (governor.on) {
		gov_every = (governor.every_ms + tick - 1) / tick;
	}
	pmic_every = (THERMAL_POLL_MS + tick - 1) / tick;
	summary_every = (60000 + tick - 1) / tick;
	if (gov_every == 0) {
		gov_every = 1;
	}
	if (pmic_every == 0) {
		pmic_every = 1;
	}
	if (summary_every == 0) {
		summary_every = 1;
	}
	for (;;) {
		if (thermal.stopped) {
			return;
		}
		if (governor.on && polls % gov_every == 0) {
			governor_sample();
		}
		if (thermal.sensors && polls % pmic_every == 0) {
			int stage[3];

			/* over SPMI: 9 register reads; the sensors are plain memory and are read every tick */
			pmic = pmic_stage(stage);
			missing = (stage[0] < 0 ? 1u : 0) | (stage[1] < 0 ? 2u : 0) | (stage[2] < 0 ? 4u : 0);
			if (pmic != last_pmic || missing != last_missing) {
				tell("thermal: vayu: PMIC alarm stages %s %d, %s %d, %s %d (-1: not read, holds the level)\n", pmic_name[0],
				    stage[0], pmic_name[1], stage[1], pmic_name[2], stage[2]);
				last_pmic = pmic;
				last_missing = missing;
			}
			sdp_check();
		}
		if (thermal.sensors) {
			int temp = 0, threshold;
			bool have = tsens_hottest(&temp, &which, &valid);
			uint32_t want = thermal.level;
			uint64_t now = now_ns();

			thermal.have = have;
			thermal.temp = have ? temp : 0;
			if (!have) {
				want = want < 2 ? 2 : want;     /* (blind: the slowest rows, and never a step back up) */
			} else {
				if (temp >= thermal.hot + 100) {
					want = 2;
				} else if (temp >= thermal.hot && want < 1) {
					want = 1;
				}
				threshold = want == 2 ? thermal.hot + 100 : thermal.hot;
				if (want > 0 && want == thermal.level && temp < threshold - 100) {
					/* level 1 -> 0 waits for the hold: the junction falls ~7 C a
					 * second here, so without it the top row is back within 2 s and
					 * the phone ran 68..91 C in the 03.11 log (see §2 of the report) */
					if (thermal.level != 1 ||
					    now - thermal.level_since >= (uint64_t)thermal.hold_ms * NSEC_PER_MSEC) {
						want--;
					}
				}
			}
			if (missing != 0 && want < thermal.level) {
				want = thermal.level;           /* (TSENS alone does not let the level down while an alarm is unread) */
			}
			if (pmic >= 1) {
				want = 2;
			}
			if (want > thermal.level) {
				thermal.level_since = now;
			}
			if (want != thermal.level) {
				if (have) {
					tell("thermal: vayu: hottest %s (tsens%u s%u) %d.%d C, PMIC stage %d: level %u -> %u\n",
					    tsens_name[which >> 8][which & 0xff], which >> 8, which & 0xff, temp / 10,
					    (temp < 0 ? -temp : temp) % 10, pmic, thermal.level, want);
				} else {
					tell("thermal: vayu: no valid TSENS sensor, PMIC stage %d: level %u -> %u\n", pmic,
					    thermal.level, want);
				}
				thermal.level = want;
			} else if (polls % summary_every == 0) {
				if (have) {
					tell("thermal: vayu: hottest %s (tsens%u s%u) %d.%d C of %u valid, PMIC stage %d, level %u\n",
					    tsens_name[which >> 8][which & 0xff], which >> 8, which & 0xff, temp / 10,
					    (temp < 0 ? -temp : temp) % 10, valid, pmic, thermal.level);
				} else {
					tell("thermal: vayu: no valid TSENS sensor, PMIC stage %d, level %u\n", pmic, thermal.level);
				}
			}
		}
		if (governor.on && governor.sum_ms != 0) {
			uint64_t now = now_ns();

			if (governor_sum_then == 0) {
				governor_sum_then = now;
			} else if (now - governor_sum_then >= (uint64_t)governor.sum_ms * NSEC_PER_MSEC) {
				governor_sum_then = now;
				governor_summary();
			}
		}
		if (governor.screen_off != 0) {
			bool hold = governor.display_off &&
			    now_ns() - governor.display_off_at >= (uint64_t)governor.screen_off_ms * NSEC_PER_MSEC;

			if (hold != governor.screen_hold) {
				governor.screen_hold = hold;
				tell("cpufreq: vayu: screen off hold %s\n", hold ? "ON: the slowest rows while iOS has the display off" :
				    "off: the display is back");
			}
		}
		{
			uint32_t signature = applied_signature();

			if (signature != thermal.applied) {
				/* (cpufreq_gov_prop: its steps are in the summary; this line only when a level changed) */
				if (!(governor.on && governor.prop) || governor.log ||
				    signature / (OSM_ROWS * OSM_ROWS * OSM_ROWS) != thermal.applied / (OSM_ROWS * OSM_ROWS * OSM_ROWS)) {
					tell("thermal: vayu: rows: temperature level %u, SDP hold %s, governor %u/%u/%u\n", thermal.level,
					    thermal.sdp_hold ? "on" : "off", governor.level[0], governor.level[1], governor.level[2]);
				}
				thermal.applied = signature;
				thermal_apply();
			}
		}
		polls++;
		delay_for_interval(tick, NSEC_PER_MSEC);
	}
}

/* The power switches of the governor (o6): cpufreq_gov_prop, cpufreq_touch_boost_pct, cpufreq_screen_off. */
static void
governor_power_init(void)
{
	uint32_t prop = 0, head = 125, up = 1, down = 3, dwell = 300, pct = 100, screen = 0, screen_ms = 5000, fix = 0;

	PE_parse_boot_argn("cpufreq_gov_prop", &prop, sizeof(prop));
	PE_parse_boot_argn("cpufreq_gov_head", &head, sizeof(head));
	PE_parse_boot_argn("cpufreq_gov_up", &up, sizeof(up));
	PE_parse_boot_argn("cpufreq_gov_down", &down, sizeof(down));
	PE_parse_boot_argn("cpufreq_gov_dwell_ms", &dwell, sizeof(dwell));
	PE_parse_boot_argn("cpufreq_touch_boost_pct", &pct, sizeof(pct));
	PE_parse_boot_argn("cpufreq_screen_off", &screen, sizeof(screen));
	PE_parse_boot_argn("cpufreq_screen_off_ms", &screen_ms, sizeof(screen_ms));
	PE_parse_boot_argn("cpufreq_touch_boost_fix", &fix, sizeof(fix));
	governor.boost_fix = fix != 0;
	governor.prop = governor.on && prop != 0;
	governor.head = head < 100 ? 100 : head > 400 ? 400 : head;
	governor.up = up < 1 ? 1 : up > 10 ? 10 : up;
	governor.down = down < 1 ? 1 : down > 20 ? 20 : down;
	governor.dwell_ms = dwell > 5000 ? 5000 : dwell;
	governor.boost_pct = pct < 20 ? 20 : pct > 100 ? 100 : pct;
	governor.screen_off = screen;
	governor.screen_off_ms = screen_ms > 600000 ? 600000 : screen_ms;
	if (governor.prop) {
		tell("cpufreq: vayu: governor: proportional (cpufreq_gov_prop=1): the busiest core's share x %u %% of the "
		    "frequency now, a step up after %u sample(s), down after %u and %u ms; touch boost to %u %% of the top\n",
		    governor.head, governor.up, governor.down, governor.dwell_ms, governor.boost_pct);
		for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
			const struct raised_domain *d = &raised[domain];

			if (d->wrote) {
				tell("cpufreq: vayu: group %u ladder: %u steps, row %u %u MHz .. row %u %u MHz%s\n", domain, d->steps,
				    d->steps ? d->ladder_row[0] : 0, d->steps ? d->ladder_khz[0] / 1000 : 0,
				    d->steps ? d->ladder_row[d->steps - 1] : 0, d->steps ? d->ladder_khz[d->steps - 1] / 1000 : 0,
				    d->steps < 2 ? " (too short: the three rows stay)" : "");
			}
		}
	}
	if (governor.boost_fix) {
		tell("cpufreq: vayu: touch boost: the hold is %u ms of real time (cpufreq_touch_boost_fix=1; without it, %u ms "
		    "was compared with 19.2 MHz ticks and held ~52 times as long)\n", governor.touch_boost_ms, governor.touch_boost_ms);
	}
	if (governor.screen_off != 0) {
		tell("cpufreq: vayu: screen off hold on (cpufreq_screen_off=1): the slowest rows %u ms after iOS turns the "
		    "display off\n", governor.screen_off_ms);
	}
}

static void
vayu_thermal_init(bool monitor)
{
	uint32_t on = 1, hot = 75, gov = 1, every = 100, hold = 3000, log = 0, log_ms = 5000, sum_ms = 10000;
	uint32_t boost = 1, boost_ms = 300;
	thread_t thread;

	PE_parse_boot_argn("vayu_thermal", &on, sizeof(on));
	PE_parse_boot_argn("vayu_thermal_hot", &hot, sizeof(hot));
	PE_parse_boot_argn("cpufreq_gov", &gov, sizeof(gov));
	PE_parse_boot_argn("cpufreq_gov_ms", &every, sizeof(every));
	PE_parse_boot_argn("cpufreq_gov_log", &log, sizeof(log));
	PE_parse_boot_argn("cpufreq_gov_log_ms", &log_ms, sizeof(log_ms));
	PE_parse_boot_argn("cpufreq_gov_sum_ms", &sum_ms, sizeof(sum_ms));
	PE_parse_boot_argn("cpufreq_touch_boost", &boost, sizeof(boost));
	PE_parse_boot_argn("cpufreq_touch_boost_ms", &boost_ms, sizeof(boost_ms));
	governor.on = !monitor && gov != 0;
	governor.touch_boost = boost != 0;
	governor.touch_boost_ms = boost_ms > 5000 ? 5000 : boost_ms;
	governor.every_ms = every < 20 ? 20 : every > 1000 ? 1000 : every;
	governor.log = log != 0;
	governor.log_ms = log_ms < 1000 ? 1000 : log_ms > 600000 ? 600000 : log_ms;
	governor.sum_ms = sum_ms > 600000 ? 600000 : sum_ms;
	governor_power_init();
	if (governor.on) {
		char more[80] = "";

		if (governor.sum_ms != 0) {
			snprintf(more, sizeof(more), ", a summary every %u ms", governor.sum_ms);
		}
		if (governor.log) {
			strlcat(more, ", a line per row change", sizeof(more));
		}
		tell("cpufreq: vayu: governor on: a sample every %u ms%s; rows 0 (top) / 1 (~3/4) / 2 (slowest)\n",
		    governor.every_ms, more);
		tell("cpufreq: vayu: touch boost %s: the top rows while a finger is down and for %u ms after it%s\n",
		    governor.touch_boost != 0 ? "on" : "off", governor.touch_boost_ms,
		    governor.touch_boost != 0 ? "" : " (cpufreq_touch_boost=0)");
	}
	thermal.hold_ms = hold > 60000 ? 60000 : hold;
	thermal.sdp_on = 1;
	thermal.sdp_below = 50;
	PE_parse_boot_argn("vayu_sdp_slow", &thermal.sdp_on, sizeof(thermal.sdp_on));
	PE_parse_boot_argn("vayu_sdp_below", &thermal.sdp_below, sizeof(thermal.sdp_below));
	thermal.sdp_below = thermal.sdp_below > 95 ? 95 : thermal.sdp_below;
	if (monitor) {
		thermal.sdp_on = 0;
	}
	if (on == 0 && !governor.on) {
		tell("thermal: vayu: off (vayu_thermal=0)\n");
		return;
	}
	thermal.sensors = on != 0;
	if (!thermal.sensors) {
		tell("thermal: vayu: off (vayu_thermal=0); the governor still runs\n");
		goto start;
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
	tell("thermal: vayu: throttle at %d C (level 1), %d C (level 2), each 10 C lower to step back, level 1 held %u ms; "
	    "the sensors every %u ms, the PMIC every %u ms\n", thermal.hot / 10, thermal.hot / 10 + 10, thermal.hold_ms,
	    THERMAL_TICK_MS, THERMAL_POLL_MS);
	for (uint32_t domain = 0; domain < OSM_DOMAINS; domain++) {
		if (raised[domain].managed) {
			tell("thermal: vayu: group %u levels: row %u %u MHz / row %u %u MHz / row %u %u MHz\n", domain,
			    raised[domain].row[0], raised[domain].khz[0] / 1000, raised[domain].row[1], raised[domain].khz[1] / 1000,
			    raised[domain].row[2], raised[domain].khz[2] / 1000);
		}
	}
start:
	thermal.applied = applied_signature();          /* (the rows are where the boot left them: no write yet) */
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
	uint32_t cores[OSM_DOMAINS] = { 0 }, dry = 0, force = 0, turbo = 0, l3 = 1, ddr = 0, here, before, monitor = 0;

	if (!cpufreq_wanted()) {
		DTEntry entry;
		void const *type;
		unsigned int size;

		/* (the addresses are vayu's: the monitor alone only on the board the loader names so in /arm-io) */
		if (!PE_parse_boot_argn("vayu_thermal_monitor", &monitor, sizeof(monitor)) || monitor == 0 ||
		    SecureDTLookupEntry(NULL, "/arm-io", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "device_type", &type, &size) != kSuccess ||
		    size < 4 || memcmp(type, "vayu", 4) != 0 || (size > 4 && ((const char *)type)[4] != 0)) {
			return;
		}
		tell("thermal: vayu: monitor only (vayu_thermal_monitor=1): no clock raise, governor or SDP hold\n");
	}
	PE_parse_boot_argn("cpufreq_dry", &dry, sizeof(dry));
	PE_parse_boot_argn("cpufreq_force", &force, sizeof(force));
	PE_parse_boot_argn("cpufreq_turbo", &turbo, sizeof(turbo));
	PE_parse_boot_argn("cpufreq_l3", &l3, sizeof(l3));
	PE_parse_boot_argn("vayu_ddr", &ddr, sizeof(ddr));
	if (monitor) {
		force = turbo = l3 = ddr = 0;
	}
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
			raise_domain(domain, cores[domain], dry != 0, force != 0, turbo != 0, monitor != 0);
		}
	}
	if (l3 != 0) {
		raise_l3(dry != 0);
	}
	if (ddr != 0) {
		vote_bandwidth(dry != 0);
	}
	if (!dry && !monitor) {
		tell("cpufreq: vayu: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	}
	vayu_thermal_init(monitor != 0);
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
		raised[domain].written = raised[domain].was;
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
		l3_raised.written = l3_raised.was;
		*(volatile uint32_t *)(l3_raised.base + OSM_PERF_STATE) = l3_raised.was;
		tell("cpufreq: vayu: L3 back to row %u for the loader\n", l3_raised.was);
	}
}
