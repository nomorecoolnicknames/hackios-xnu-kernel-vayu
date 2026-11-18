#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdarg.h>
#include <sched.h>
typedef uintptr_t vm_offset_t;
#define NSEC_PER_SEC 1000000000ULL
#define LCK_GRP_DECLARE(name, desc) int name __attribute__((unused))
#define LCK_SPIN_DECLARE(name, group) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
#define lck_spin_lock pthread_mutex_lock
#define lck_spin_unlock pthread_mutex_unlock
static uint64_t tick;
static uint64_t mach_absolute_time(void) { return tick; }
static void absolutetime_to_nanoseconds(uint64_t at, uint64_t *ns) { *ns = at; }
static int battery_temperature_c;
static bool forcing, broken, lit;
static uint32_t v2_leds = 3;
#define V2_EN_LED_CTRL 0x4c
#define V2_MITIGATION_SW 0x6f
#define V2_MOD_CTRL 0x46
#define V2_TGR_CURRENT(n) (0x43 + (n))
#define V2_SAFETY_TMR(n) (0x40 + (n))
#define V2_SAFETY_TMR_OFF 0x13
static unsigned attempted[256], results, fail_reg;
static bool put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    (void)mask; (void)value; attempted[reg]++;
    if (reg == fail_reg) { broken = true; return false; }
    return !broken || forcing;
}
static void say(const char *format, ...) { (void)format; results++; }
struct peripheral { const char *name; uint16_t id, channel; bool found; };
#define ARB_FEATURES 4
#define ARB_CHANNEL(n) (0x900 + 4 * (n))
#define ARB_MAX_CHANNELS 512
static uint32_t arb_regs[0x1100 / 4];
static uintptr_t arb_core = (uintptr_t)arb_regs, arb_observer = 1;
static uint32_t rd(uintptr_t at) { sched_yield(); return *(uint32_t *)at; }
static pthread_mutex_t bus_lock = PTHREAD_MUTEX_INITIALIZER;
static void hakios_spmi_lock(void) { assert(!pthread_mutex_lock(&bus_lock)); }
static void hakios_spmi_unlock(void) { assert(!pthread_mutex_unlock(&bus_lock)); }
static const char *spmi_read(const struct peripheral *p, uint8_t reg, uint8_t *value, uint32_t count)
{
    (void)reg; (void)count;
    hakios_spmi_lock();
    assert(p->id == p->channel + 1); *value = p->id;
    hakios_spmi_unlock(); return NULL;
}
static struct peripheral flash, alarm;
static uint8_t alarm_subtype;
static bool v2 = true;
#define FLASH_STATUS1 0x06
#define FLASH_STATUS2 0x07
static bool get(const struct peripheral *p, uint8_t reg, uint8_t *value)
{ (void)p; (void)reg; *value = 0; return true; }
#define note say
#define tell say
typedef void *DTEntry;
#define kSuccess 0
static const char *board = "redwood";
static uint32_t cpufreq_on = 1, cpufreq_vayu;
static bool PE_parse_boot_argn(const char *name, void *out, size_t size)
{ assert(size == 4); *(uint32_t *)out = strcmp(name, "cpufreq") == 0 ? cpufreq_on : cpufreq_vayu; return true; }
static int SecureDTLookupEntry(void *root, const char *path, DTEntry *entry)
{ (void)root; assert(!strcmp(path, "/arm-io")); *entry = (void *)board; return kSuccess; }
static int SecureDTGetProperty(DTEntry entry, const char *name, const void **prop, unsigned *size)
{ (void)entry; assert(!strcmp(name, "device_type")); *prop = board; *size = strlen(board) + 1; return kSuccess; }
#include "guards.inc"
static void *reader(void *parameter)
{
    uintptr_t id = (uintptr_t)parameter;
    for (unsigned n = 0; n < 1000; n++) {
        uint8_t value;
        assert(hakios_spmi_peek(id, 0x04, &value) && value == id);
    }
    return NULL;
}
int main(void)
{
    assert(hakios_battery_temperature() == -273);
    battery_loader_temperature_update(10, 30);
    assert(hakios_battery_temperature() == -273);
    tick = 31 * NSEC_PER_SEC;
    battery_loader_temperature_update(10, 30);
    assert(hakios_battery_temperature() == -273);
    battery_loader_temperature_update(12, 31);
    assert(hakios_battery_temperature() == 31);
    tick += 31 * NSEC_PER_SEC;
    battery_loader_temperature_update(12, 31);
    assert(hakios_battery_temperature() == -273);
    tick = NSEC_PER_SEC;
    battery_temperature_update(30); assert(hakios_battery_temperature() == 30);
    tick += 30 * NSEC_PER_SEC; assert(hakios_battery_temperature() == 30);
    tick++; assert(hakios_battery_temperature() == -273);
    battery_temperature_update(-273); assert(hakios_battery_temperature() == -273);
    battery_temperature_update(50); assert(hakios_battery_temperature() == 50);
    assert(trouble() != NULL);
    battery_temperature_update(-273); assert(strstr(trouble(), "unknown or stale"));
    battery_temperature_update(30); assert(trouble() == NULL);
    tick += 31 * NSEC_PER_SEC; assert(strstr(trouble(), "unknown or stale"));
    assert(cpufreq_wanted());
    board = "vayu"; assert(!cpufreq_wanted());
    cpufreq_vayu = 1; assert(!cpufreq_wanted());
    board = "redwood"; cpufreq_vayu = cpufreq_on = 0; assert(!cpufreq_wanted());
    for (unsigned reg = 0; reg < 3; reg++) {
        memset(attempted, 0, sizeof(attempted)); results = 0;
        fail_reg = reg == 0 ? V2_EN_LED_CTRL : reg == 1 ? V2_MITIGATION_SW : V2_MOD_CTRL;
        broken = lit = true;
        assert(!v2_dark() && !forcing && lit);
        assert(attempted[V2_EN_LED_CTRL] == 1 && attempted[V2_MITIGATION_SW] == 1 && attempted[V2_MOD_CTRL] == 1);
        assert(attempted[V2_TGR_CURRENT(0)] == 1 && attempted[V2_TGR_CURRENT(1)] == 1);
        assert(results == 3);
    }
    fail_reg = 0; assert(v2_dark() && !lit);
    arb_regs[ARB_FEATURES / 4] = 32;
    for (unsigned i = 0; i < 32; i++) arb_regs[ARB_CHANNEL(i) / 4] = (i + 1) << 8;
    pthread_t threads[16];
    for (uintptr_t i = 0; i < 16; i++) assert(!pthread_create(&threads[i], NULL, reader, (void *)(i + 1)));
    for (unsigned i = 0; i < 16; i++) assert(!pthread_join(threads[i], NULL));
    uint8_t value;
    assert(!hakios_spmi_peek(17, 0x04, &value));
    assert(hakios_spmi_peek(1, 0x04, &value) && value == 1);
    puts("power-test: stale battery, independent off attempts, concurrent SPMI cache and capacity OK");
}
