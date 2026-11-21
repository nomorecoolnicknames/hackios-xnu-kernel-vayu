/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * Host test of osfmk/arm64/gic_v3_irq.c (VAYU), on a fake distributor (gic_host_shim.h):
 *   sgi1r   ICC_SGI1R_EL1 for Aff0 (QEMU's cores), Aff1 (vayu's: 0x700 ... 0x100), Aff2, and the refusals
 *   api     attach/detach/mask/unmask/count, GICD_TYPER's limit, the errors
 *   aba     IRQ threads take the line while another thread attaches it with a fresh context, detaches it and frees
 *           that context, again and again: no handler may see a context after its detach returned, nor another
 *           driver's (ASan finds a use after free; the handler checks the context's generation)
 *   storm   an SPI nobody attached is masked and counted
 *   selfdetach  a detach from the interrupt's own handler panics (a child process)
 *   build:  cc -O1 -g -fsanitize=address,undefined -pthread -I. gic_host_test.c -o gic_host_test   (run.sh)
 */
#define VAYU_GIC_HOST_TEST 1
#include "gic_host_shim.h"
#include "../../../osfmk/arm64/gic_v3_irq.c"

#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

pthread_spinlock_t host_spi_lock;
__thread int host_in_irq, host_irq_off;
uint32_t host_gicd[0x10000 / 4];
uint32_t host_enabled[32];

uint32_t
host_r32(vm_offset_t a)
{
	uintptr_t off = a - (uintptr_t)host_gicd;
	if (off >= 0x100 && off < 0x180) {
		return __atomic_load_n(&host_enabled[(off - 0x100) / 4], __ATOMIC_SEQ_CST);
	}
	return __atomic_load_n((volatile uint32_t *)a, __ATOMIC_SEQ_CST);
}

void
host_w32(vm_offset_t a, uint32_t v)
{
	uintptr_t off = a - (uintptr_t)host_gicd;
	if (off >= 0x100 && off < 0x180) {
		__atomic_or_fetch(&host_enabled[(off - 0x100) / 4], v, __ATOMIC_SEQ_CST);
	} else if (off >= 0x180 && off < 0x200) {
		__atomic_and_fetch(&host_enabled[(off - 0x180) / 4], ~v, __ATOMIC_SEQ_CST);
	} else {
		__atomic_store_n((volatile uint32_t *)a, v, __ATOMIC_SEQ_CST);
	}
}

static ml_topology_cpu_t boot = { 0x700 };
static ml_topology_info_t topo = { &boot };
const ml_topology_info_t *ml_get_topology_info(void) { return &topo; }

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static bool
enabled(uint32_t intid)
{
	return (host_enabled[intid / 32] >> (intid % 32)) & 1;
}

static void
irq(uint32_t intid)
{
	host_in_irq = 1;
	host_irq_off = 1;
	vayu_gic_spi_handle(intid);
	host_in_irq = 0;
	host_irq_off = 0;
}

/* ---- sgi1r ---- */
static void
test_sgi1r(void)
{
	CHECK(vayu_gic_sgi1r(0x000, 1) == 0x0000000001000001ULL);
	CHECK(vayu_gic_sgi1r(0x700, 1) == 0x0000000001070001ULL);      /* vayu core 7: Aff1 7, Aff0 0 */
	CHECK(vayu_gic_sgi1r(0x600, 2) == 0x0000000002060001ULL);
	CHECK(vayu_gic_sgi1r(0x100, 1) == 0x0000000001010001ULL);
	for (uint32_t c = 1; c < 8; c++) {                               /* the seven cores the loader gives */
		uint64_t v = vayu_gic_sgi1r(c << 8, 1);
		CHECK(((v >> 16) & 0xff) == c && (v & 0xffff) == 1 && ((v >> 24) & 0xf) == 1);
	}
	CHECK(vayu_gic_sgi1r(0x003, 1) == 0x0000000001000008ULL);      /* QEMU core 3: Aff0 */
	CHECK(vayu_gic_sgi1r(0x00f, 2) == 0x0000000002008000ULL);
	CHECK(vayu_gic_sgi1r(0x010203, 1) == ((1ULL << 32) | (1ULL << 24) | (2ULL << 16) | (1ULL << 3)));
	CHECK(vayu_gic_sgi1r(0x010, 1) == ~0ULL);                       /* Aff0 16: range selector */
	CHECK(vayu_gic_sgi1r(0x000, 16) == ~0ULL);
}

/* ---- api ---- */
static int api_calls;
static void api_handler(void *ctx, uint32_t intid) { (void)intid; CHECK(ctx == &api_calls); api_calls++; }

static void
test_api(void)
{
	host_gicd[1] = 3;                       /* GICD_TYPER.ITLinesNumber 3: INTIDs 0..127 */
	CHECK(gic_v3_spi_attach(31, false, api_handler, &api_calls) == KERN_INVALID_ARGUMENT);
	CHECK(gic_v3_spi_attach(128, false, api_handler, &api_calls) == KERN_INVALID_ARGUMENT);
	CHECK(gic_v3_spi_attach(40, false, NULL, NULL) == KERN_INVALID_ARGUMENT);
	CHECK(gic_v3_spi_attach(127, true, api_handler, &api_calls) == KERN_SUCCESS);
	CHECK(((host_gicd[0xc00 / 4 + 127 / 16] >> ((127 % 16) * 2)) & 3) == 2);   /* edge */
	CHECK(*(uint64_t *)((uint8_t *)host_gicd + 0x6000 + 8 * 127) == 0x700);   /* routed to the boot CPU */
	CHECK(gic_v3_spi_attach(127, true, api_handler, &api_calls) == KERN_NAME_EXISTS);
	CHECK(enabled(127));
	irq(127);
	irq(127);
	CHECK(api_calls == 2 && gic_v3_spi_count(127) == 2);
	gic_v3_spi_mask(127);
	CHECK(!enabled(127));
	gic_v3_spi_unmask(127);
	CHECK(enabled(127));
	gic_v3_spi_mask(126);                   /* not attached: not the caller's */
	gic_v3_spi_detach(127);
	CHECK(!enabled(127) && gic_v3_spi_count(127) == 0);
	irq(127);                               /* after the detach: masked, counted as nobody's, no handler */
	CHECK(api_calls == 2);
	/* every slot, then one too many */
	for (uint32_t i = 0; i < VAYU_SPI_SLOTS; i++) {
		CHECK(gic_v3_spi_attach(32 + i, false, api_handler, &api_calls) == KERN_SUCCESS);
	}
	CHECK(gic_v3_spi_attach(32 + VAYU_SPI_SLOTS, false, api_handler, &api_calls) == KERN_RESOURCE_SHORTAGE);
	for (uint32_t i = 0; i < VAYU_SPI_SLOTS; i++) {
		gic_v3_spi_detach(32 + i);
	}
	host_gicd[1] = 31;                      /* the GIC-600's 1020 */
}

/* ---- storm ---- */
static void
test_storm(void)
{
	uint64_t before = vayu_gic_unhandled_spi;
	host_enabled[300 / 32] |= 1u << (300 % 32);     /* left on by the firmware */
	irq(300);
	CHECK(!enabled(300) && vayu_gic_unhandled_spi == before + 1);
}

/* ---- aba ---- */
struct aba_ctx {
	uint32_t magic;
	uint32_t gen;
	uint32_t alive;
};
#define ABA_MAGIC 0x5a5a1234u
static volatile uint32_t aba_gen;               /* the generation attached now (0: none) */
static volatile int aba_stop;
static uint64_t aba_handled, aba_bad;

static void
aba_handler(void *c, uint32_t intid)
{
	struct aba_ctx *ctx = c;
	CHECK(intid == 77);
	if (ctx->magic != ABA_MAGIC || !__atomic_load_n(&ctx->alive, __ATOMIC_SEQ_CST)) {
		__atomic_add_fetch(&aba_bad, 1, __ATOMIC_SEQ_CST);
	}
	/* a little work while a detach may be waiting */
	for (volatile int i = 0; i < 50; i++) {
	}
	if (ctx->gen != __atomic_load_n(&aba_gen, __ATOMIC_SEQ_CST) && __atomic_load_n(&aba_gen, __ATOMIC_SEQ_CST) != 0) {
		/* a handler of the old driver while the new one is attached: what the 8796 table allowed */
		__atomic_add_fetch(&aba_bad, 1, __ATOMIC_SEQ_CST);
	}
	__atomic_add_fetch(&aba_handled, 1, __ATOMIC_SEQ_CST);
}

static void *
aba_irq_thread(void *arg)
{
	(void)arg;
	while (!aba_stop) {
		irq(77);
	}
	return NULL;
}

static void
test_aba(void)
{
	pthread_t t[3];
	for (int i = 0; i < 3; i++) {
		pthread_create(&t[i], NULL, aba_irq_thread, NULL);
	}
	for (uint32_t g = 1; g <= 20000; g++) {
		struct aba_ctx *ctx = malloc(sizeof(*ctx));
		ctx->magic = ABA_MAGIC;
		ctx->gen = g;
		ctx->alive = 1;
		__atomic_store_n(&aba_gen, g, __ATOMIC_SEQ_CST);
		CHECK(gic_v3_spi_attach(77, false, aba_handler, ctx) == KERN_SUCCESS);
		/* other lines come and go too, so the slots move */
		CHECK(gic_v3_spi_attach(78 + (g % 5), false, api_handler, &api_calls) == KERN_SUCCESS);
		gic_v3_spi_detach(78 + (g % 5));
		gic_v3_spi_detach(77);
		__atomic_store_n(&aba_gen, 0, __ATOMIC_SEQ_CST);
		__atomic_store_n(&ctx->alive, 0, __ATOMIC_SEQ_CST);
		ctx->magic = 0;
		free(ctx);                      /* ASan: any handler touching it now is a use after free */
	}
	aba_stop = 1;
	for (int i = 0; i < 3; i++) {
		pthread_join(t[i], NULL);
	}
	CHECK(aba_bad == 0);
	CHECK(aba_handled > 0);
	printf("aba: %llu handler runs over 20000 attach/detach cycles, %llu bad\n",
	    (unsigned long long)aba_handled, (unsigned long long)aba_bad);
}

/* ---- self detach ---- */
static void self_detach_handler(void *c, uint32_t intid) { (void)c; gic_v3_spi_detach(intid); }

static void
test_self_detach(void)
{
	pid_t p = fork();
	if (p == 0) {
		if (freopen("/dev/null", "w", stderr) == NULL) {
			_exit(2);
		}
		gic_v3_spi_attach(90, false, self_detach_handler, NULL);
		irq(90);
		_exit(0);                       /* not reached: the detach panics */
	}
	int st = 0;
	waitpid(p, &st, 0);
	CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT);
}

int
main(void)
{
	pthread_spin_init(&host_spi_lock, PTHREAD_PROCESS_PRIVATE);
	host_gicd[1] = 31;
	test_sgi1r();
	test_api();
	test_storm();
	test_self_detach();
	test_aba();
	printf("gic_host_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
