#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef uintptr_t vm_offset_t;
typedef int wait_result_t;
#define __unused __attribute__((unused))
#define NSEC_PER_MSEC 1000000
#define MAX_CPUS 8
static uint32_t registers[4][0x1400 / 4];
static bool sensor_valid;
static int sensor_temperature, alarm_stage[3];
static void tell(const char *fmt, ...) { (void)fmt; }
static uintptr_t ml_io_map(uintptr_t pa, size_t size)
{ (void)size; return (uintptr_t)registers[pa == 0x18323000 ? 0 : pa == 0x18325800 ? 1 : pa == 0x18327800 ? 2 : 3]; }
static void delay_for_interval(uint32_t count, uint32_t unit);
static void governor_sample(void) {}
static void governor_summary(void) {}
static void sdp_check(void) {}
static uint64_t clock_ns;
static uint64_t mach_absolute_time(void) { return clock_ns; }
static void absolutetime_to_nanoseconds(uint64_t at, uint64_t *ns) { *ns = at; }
static bool tsens_hottest(int *temp, uint32_t *which, uint32_t *valid)
{ *temp = sensor_temperature; *which = 0; *valid = sensor_valid; return sensor_valid; }
static int pmic_stage(int stage[3])
{ int most = -1; for (int i = 0; i < 3; i++) { stage[i] = alarm_stage[i]; if (stage[i] > most) most = stage[i]; } return most; }
#include "thermal.inc"
/* thermal_apply()'s 20 ms after a write is not the loop's tick: one pass of the loop, then stop */
static void delay_for_interval(uint32_t count, uint32_t unit)
{ (void)unit; clock_ns += (uint64_t)count * NSEC_PER_MSEC; if (count != 20) thermal.stopped = true; }

static void setup(uint32_t boot_row)
{
    memset(registers, 0, sizeof(registers));
    memset(raised, 0, sizeof(raised));
    memset(&thermal, 0, sizeof(thermal));
    memset(&governor, 0, sizeof(governor));
    memset(&l3_raised, 0, sizeof(l3_raised));
    registers[0][OSM_ENABLE / 4] = 1;
    registers[0][OSM_PERF_STATE / 4] = boot_row;
    for (unsigned i = 0; i < OSM_ROWS; i++) {
        unsigned multiplier = i == 0 ? 20 : i == 1 ? 40 : 80;
        registers[0][(OSM_FREQ_LUT + i * OSM_ROW_SIZE) / 4] = (1u << 30) | (4 << 16) | multiplier;
    }
    sensor_valid = true; sensor_temperature = 400;
    alarm_stage[0] = alarm_stage[1] = alarm_stage[2] = 0;
    thermal.sensors = true; thermal.hot = 850;
}
static void poll(uint32_t level)
{
    thermal.level = level;
    thermal.applied = applied_signature();
    thermal.stopped = false;
    thermal_thread(NULL, 0);
}
int main(void)
{
    setup(2);
    uint32_t before[sizeof(registers) / sizeof(uint32_t)];
    memcpy(before, registers, sizeof(registers));
    raise_domain(0, 4, false, false, false, false);
    assert(raised[0].managed && !raised[0].wrote);
    assert(!memcmp(before, registers, sizeof(registers)));
    sensor_temperature = 960; poll(0);
    assert(thermal.level == 2 && registers[0][OSM_PERF_STATE / 4] == 0 && raised[0].wrote);
    sensor_valid = false; poll(2);
    assert(thermal.level == 2 && registers[0][OSM_PERF_STATE / 4] == 0);
    poll(0); assert(thermal.level == 2);
    sensor_valid = true; sensor_temperature = 400;
    alarm_stage[0] = alarm_stage[1] = alarm_stage[2] = -1;
    poll(2); assert(thermal.level == 2);
    alarm_stage[0] = alarm_stage[1] = 0;
    poll(2); assert(thermal.level == 2);
    alarm_stage[2] = 0;
    poll(2); assert(thermal.level == 1 && registers[0][OSM_PERF_STATE / 4] == 1);
    poll(1); assert(thermal.level == 0 && registers[0][OSM_PERF_STATE / 4] == 2);
    alarm_stage[0] = 1; poll(0); assert(thermal.level == 2);
    vayu_cpufreq_quiesce(); assert(registers[0][OSM_PERF_STATE / 4] == 2);

    setup(1); memcpy(before, registers, sizeof(registers));
    raise_domain(0, 4, false, false, false, true);
    assert(raised[0].managed && !raised[0].wrote && raised[0].row[0] == 1);
    assert(!memcmp(before, registers, sizeof(registers)));
    poll(0); assert(!memcmp(before, registers, sizeof(registers)));
    sensor_temperature = 960; poll(0); assert(registers[0][OSM_PERF_STATE / 4] == 0);
    sensor_temperature = 400; poll(2); poll(1); assert(registers[0][OSM_PERF_STATE / 4] == 1);
    setup(2); memcpy(before, registers, sizeof(registers));
    raise_domain(0, 4, true, false, false, false);
    sensor_temperature = 960; poll(0);
    assert(!raised[0].managed && !memcmp(before, registers, sizeof(registers)));
    setup(0); raise_domain(0, 4, false, false, false, false);
    assert(raised[0].wrote && registers[0][OSM_PERF_STATE / 4] == 2);
    setup(2); memcpy(registers[3], registers[0], sizeof(registers[3]));
    memcpy(before, registers, sizeof(registers));
    raise_l3(false); assert(l3_raised.managed && !l3_raised.wrote);
    assert(!memcmp(before, registers, sizeof(registers)));
    sensor_temperature = 960; poll(0); assert(registers[3][OSM_PERF_STATE / 4] == 0);
    sensor_temperature = 400; poll(2); poll(1); assert(registers[3][OSM_PERF_STATE / 4] == 2);
    setup(1); memcpy(registers[3], registers[0], sizeof(registers[3]));
    raise_l3(false); assert(l3_raised.wrote && registers[3][OSM_PERF_STATE / 4] == 2);
    sensor_temperature = 960; poll(0); assert(registers[3][OSM_PERF_STATE / 4] == 1);
    puts("power-test: inherited CPU/L3 rows, cold monitor, dry run, sensor loss and recovery OK");
}
