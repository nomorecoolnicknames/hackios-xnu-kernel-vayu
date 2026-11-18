#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
typedef uintptr_t vm_offset_t;
typedef int lck_mtx_t;
struct vayu_rproc { const char *name; uint16_t smem_host; };
#define VAYU_SMEM_HOST_APPS 0
#define VAYU_SMEM_HOST_GLOBAL 0xfffe
#define VM_WIMG_WCOMB 0
#define ITEM_GLINK_THEIR_QUEUE 480
#define Z_WAITOK 0
#define GENERIC_PSCI_SMP 1
static uint8_t memory[0x200000];
static unsigned logs;
static void say(const char *fmt, ...) { (void)fmt; logs++; }
static uintptr_t ml_io_map_unmappable(uintptr_t pa, size_t size, unsigned flags)
{ (void)pa; (void)size; (void)flags; return (uintptr_t)memory; }
static void wr(uintptr_t at, uint32_t word) { memcpy((void *)at, &word, 4); }
static void *kalloc_data(size_t size, int flags) { (void)flags; return malloc(size); }
#define strlcpy test_strlcpy
static size_t test_strlcpy(char *to, const char *from, size_t n)
{ size_t len = strlen(from); if (n) { size_t use = len < n - 1 ? len : n - 1; memcpy(to, from, use); to[use] = 0; } return len; }
static void vayu_apcs_ring(unsigned bit) { (void)bit; }
#include "pas.inc"

static void setup_partition(void)
{
    memset(memory, 0, sizeof(memory));
    struct smem_table table = { .magic = { '$', 'T', 'O', 'C' }, .version = 1, .count = 1 };
    struct smem_table_entry toc = { .offset = 0x1000, .size = 0x4000, .host0 = 0, .host1 = 2, .cacheline = 64 };
    struct smem_partition part = { .magic = { '$', 'P', 'R', 'T' }, .size = 0x4000,
        .free_uncached = 32 + 16 + 2048, .free_cached = 0x4000 };
    struct smem_entry entry = { .canary = SMEM_CANARY, .item = 480, .size = 2048 };
    memcpy(memory + SMEM_SIZE - 0x1000, &table, sizeof(table));
    memcpy(memory + SMEM_SIZE - 0x1000 + sizeof(table), &toc, sizeof(toc));
    memcpy(memory + 0x1000, &part, sizeof(part));
    memcpy(memory + 0x1020, &entry, sizeof(entry));
    smem = memory;
}
static struct vayu_glink queue(void)
{
    static struct vayu_rproc rp = { "test", 2 };
    struct vayu_glink g = { .rp = &rp, .up = true, .descriptor = 0x100,
        .thread_wanted = true };
    assert(glink_theirs_queue(&g));
    return g;
}
static void message(struct vayu_glink *g, uint16_t command, uint32_t param2, uint32_t payload, uint32_t available)
{
    struct glink_message msg = { command, 1, param2 };
    memcpy(memory + g->theirs, &msg, sizeof(msg));
    memcpy(memory + g->theirs + 8, &payload, 4);
    vayu_smem_poke(g->descriptor + GLINK_THEIR_TAIL, 0);
    vayu_smem_poke(g->descriptor + GLINK_THEIR_HEAD, available);
    glink_pump(g);
}
int main(void)
{
    uint32_t size, value;
    bool bad;
    setup_partition();
    assert(vayu_smem_item(2, 480, &size) == 0x1030 && size == 2048);
    assert(!vayu_smem_peek(UINT64_MAX - 3, &value, 8));
    assert(!vayu_smem_peek(SMEM_SIZE - 3, &value, 4));
    vayu_smem_poke(UINT32_MAX - 2, 1);
    struct vayu_glink g = queue();
    message(&g, GLINK_OPEN, 0xfffffff8, 0, 8);
    assert(g.broken && !g.up && !g.thread_wanted);
    g = queue(); message(&g, GLINK_INTENT, 0x20000000, 0, 8); assert(g.broken);
    g = queue(); message(&g, GLINK_DATA, 1, 0xfffffff8, 16); assert(g.broken);
    g = queue(); message(&g, GLINK_DATA_MORE, 1, UINT32_MAX, 16); assert(g.broken);
    g = queue(); message(&g, GLINK_OPEN, 10, 0, 8);
    assert(!g.broken && vayu_smem_word(g.descriptor + GLINK_THEIR_TAIL) == 0);
    g = queue(); message(&g, GLINK_SIGNALS, 0, 0, 8);
    assert(!g.broken && vayu_smem_word(g.descriptor + GLINK_THEIR_TAIL) == 8);
    g = queue(); message(&g, GLINK_OPEN, 9, 0, 24);
    assert(!g.broken && vayu_smem_word(g.descriptor + GLINK_THEIR_TAIL) == 24);
    g = queue(); vayu_smem_poke(g.descriptor + GLINK_THEIR_HEAD, 9); glink_pump(&g); assert(g.broken);
    g = queue(); vayu_smem_poke(g.descriptor + GLINK_THEIR_HEAD, g.theirs_size); glink_pump(&g); assert(g.broken);
    struct smem_entry *entry = (void *)(memory + 0x1020);
    entry->size = 0; entry->padding_data = 1;
    bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    g = (struct vayu_glink){ .rp = g.rp, .up = true, .thread_wanted = true };
    assert(!glink_theirs_queue(&g) && g.broken);
    setup_partition(); entry->padding_header = UINT16_MAX;
    bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    setup_partition(); entry->size = UINT32_MAX;
    bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    setup_partition();
    struct smem_partition *part = (void *)(memory + 0x1000);
    part->size = UINT32_MAX;
    bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    setup_partition(); part->free_uncached = sizeof(*part); part->free_cached = 0x4000 - 64 - 2048;
    entry = (void *)(memory + 0x1000 + 0x4000 - 64);
    *entry = (struct smem_entry){ .canary = SMEM_CANARY, .item = 480, .size = 2048, .padding_data = 4, .padding_header = 48 };
    bad = false; assert(smem_find(0x1000, 64, 480, &size, &bad) == 0x1000 + part->free_cached && size == 2044 && !bad);
    entry->padding_data = 2049; bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    /* a cached entry's padding_header is not used (the item is below the header, as Linux takes it): not held against it */
    entry->padding_data = 0; entry->padding_header = 49;
    bad = false; assert(smem_find(0x1000, 64, 480, &size, &bad) == 0x1000 + part->free_cached && size == 2048 && !bad);
    entry->padding_header = 0; entry->size = 2049;
    bad = false; assert(!smem_find(0x1000, 64, 480, &size, &bad) && bad);
    entry->size = 2048; bad = false; assert(!smem_find(0x1000, 0x80000000, 480, &size, &bad) && bad);
    /* no cache line in the table: the cached list is not walked, as before -- not found, not corrupt */
    bad = false; assert(!smem_find(0x1000, 0, 480, &size, &bad) && !bad);

    enum vayu_rsc_tcs_result first;
    interrupts = true; quiet = leaving = false; busy = true;
    first = vayu_rsc_tcs_lock("timeout"); assert(first == VAYU_RSC_TCS_FAILED);
    busy = false; assert(vayu_rsc_tcs_lock("next") == VAYU_RSC_TCS_HELD);
    assert(first == VAYU_RSC_TCS_FAILED); vayu_rsc_tcs_unlock();
    interrupts = false; assert(vayu_rsc_tcs_lock("irq-off") == VAYU_RSC_TCS_SHUTDOWN);   /* (unlocked, as before; said in the log) */
    quiet = true; assert(vayu_rsc_tcs_lock("shutdown") == VAYU_RSC_TCS_SHUTDOWN);
    assert(logs > 0);
    puts("power-test: SMEM, GLINK and RPMh checks OK");
}
