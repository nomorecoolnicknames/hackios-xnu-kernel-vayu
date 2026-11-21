/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * Host test of osfmk/arm64/psci_smp.c's way back to the loader (VAYU): seven CPUs of the phone's ADT (0x700 ... 0x100)
 * as threads on a fake PSCI firmware (psci_host_shim.h). The loader gets the machine back (the stub is "jumped to")
 * only when vayu_smp_stop_others() says every other CPU is off; each case says what the answer must be:
 *   good         all take the stop                                         -> back, every CPU OFF
 *   handover     the return starts on a secondary: the boot CPU does it    -> back, from the boot CPU
 *   deaf         a CPU spins with interrupts off and never polls            -> NOT back (fail-closed)
 *   offrefused   CPU_OFF is refused on one CPU (it parks in WFE)            -> NOT back, OFF_FAILED
 *   neverarrived CPU_ON said 0, the CPU never reached C, the firmware: ON   -> NOT back
 *   wentoff      CPU_ON said 0, never reached C, the firmware: OFF         -> back
 *   noaffinity   AFFINITY_INFO not supported                               -> NOT back (off is not proven)
 *   onrefused    CPU_ON refused: the CPU never ran                         -> back
 *   alreadyon    CPU_ON said ALREADY_ON: running something, not ours        -> NOT back
 *   single       no CPU ever asked                                         -> back at once
 * K2b, the arrival watch (vayu_smp_arrival, the checkpoints start.s leaves in the CPU's slot):
 *   abandoned    CPU_ON 0, not one checkpoint in vayu_smp_arrive_ms         -> given up (NEVER), back (fail-safe)
 *   lategate     ... and it comes after: it reads its gate closed           -> switches itself off, touches nothing
 *   stuckearly   checkpoint 4 (reset_vector) and no further                 -> WAIT; NOT back (it did start)
 *   earlyfault   an exception at checkpoint 2, then its CPU_OFF             -> GONE, back (the firmware: OFF)
 *   parked       ... but its CPU_OFF was refused                            -> PARKED, NOT back
 */
#define VAYU_PSCI_HOST_TEST 1
#include "psci_host_shim.h"
#include "../../../osfmk/arm64/psci_smp.c"

#include <unistd.h>

ml_topology_info_t host_topology;
__thread int host_cpu;
uint64_t vayu_gic_stray_stop;

enum { CPU_NORMAL, CPU_DEAF, CPU_OFF_REFUSED, CPU_NO_THREAD };
static int cpu_mode[MAX_CPUS];
static volatile int fw_power[MAX_CPUS];         /* 1: on */
static int fw_affinity_unsupported;
static int32_t fw_cpu_on_rc[MAX_CPUS];
static volatile int sgi2_pending[MAX_CPUS];
static volatile int quit;
static volatile int returned_jump, returned_fail, returned_cpu = -1;
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static int
logical_of(uint64_t phys)
{
	for (unsigned int i = 0; i < host_topology.num_cpus; i++) {
		if (host_topology.cpus[i].phys_id == phys) {
			return (int)i;
		}
	}
	return -1;
}

int64_t
host_psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	(void)a2; (void)a3;
	if (function == 0xC4000003u) {                  /* CPU_ON */
		int l = logical_of(a1);
		if (fw_cpu_on_rc[l] == 0) {
			fw_power[l] = 1;
		}
		return fw_cpu_on_rc[l];
	}
	if (function == 0x84000002u) {                  /* CPU_OFF, of the caller */
		if (cpu_mode[host_cpu] == CPU_OFF_REFUSED) {
			return VAYU_PSCI_DENIED;
		}
		fw_power[host_cpu] = 0;
		pthread_exit(NULL);                     /* does not return */
	}
	if (function == 0xC4000004u) {                  /* AFFINITY_INFO */
		if (fw_affinity_unsupported) {
			return VAYU_PSCI_NOT_SUPPORTED;
		}
		int l = logical_of(a1);
		return l < 0 ? VAYU_PSCI_INVALID_PARAMETERS : (fw_power[l] ? VAYU_PSCI_AFFINITY_ON : VAYU_PSCI_AFFINITY_OFF);
	}
	return VAYU_PSCI_NOT_SUPPORTED;
}

void
host_wfe(void)
{
	/* only a CPU that has left the kernel waits here (parked after CPU_OFF refused, or the hang test) */
	for (;;) {
		if (quit) {
			pthread_exit(NULL);
		}
		usleep(100);
	}
}

bool
vayu_gic_send_sgi(uint32_t phys_id, uint32_t sgi)
{
	int l = logical_of(phys_id);
	if (l >= 0 && sgi == VAYU_SGI_STOP) {
		sgi2_pending[l] = 1;
	}
	return l >= 0;
}

void vayu_gic_cpu_quiesce(bool all_spis, bool local_off) { (void)all_spis; (void)local_off; }
void loader_log_line(const char *line) { if (getenv("V")) { fputs(line, stdout); } }
void loader_log_sync(void) { }

/* the real one gives the machine back only on true; here: which it would have done */
void
loader_return(void)
{
	bool ok = vayu_smp_stop_others();
	returned_cpu = host_cpu;
	if (ok) {
		returned_jump = 1;
	} else {
		returned_fail = 1;
	}
}

static void *
cpu_thread(void *arg)
{
	host_cpu = (int)(intptr_t)arg;
	vayu_smp_cpu_running((unsigned int)host_cpu);
	while (!quit) {
		if (cpu_mode[host_cpu] == CPU_DEAF) {
			usleep(100);    /* interrupts off for good: takes neither SGI 2 nor the poll */
			continue;
		}
		if (sgi2_pending[host_cpu]) {
			sgi2_pending[host_cpu] = 0;
			vayu_smp_sgi_stop();    /* sleh_irq(): SGI 2 after its EOI */
		}
		vayu_smp_poll();
		usleep(50);
	}
	return NULL;
}

static pthread_t threads[MAX_CPUS];

/* start the case: seven CPUs, the boot CPU (logical 0, 0x700) is this thread or a thread of its own */
static void
setup(int ncpus)
{
	quit = 0;
	returned_jump = returned_fail = 0;
	returned_cpu = -1;
	fw_affinity_unsupported = 0;
	memset((void *)sgi2_pending, 0, sizeof(sgi2_pending));
	memset(vayu_cpu_state, 0, sizeof(vayu_cpu_state));
	vayu_cpus_asked = 0;
	vayu_stopper = -1;
	vayu_stop_asked = 0;
	vayu_return_asker = -1;
	vayu_smp_on = true;
	host_topology.num_cpus = (unsigned int)ncpus;
	for (int i = 0; i < ncpus; i++) {
		host_topology.cpus[i].cpu_id = (uint32_t)i;
		host_topology.cpus[i].phys_id = (uint32_t)(7 - i) << 8;
		fw_power[i] = i == 0;
	}
	host_topology.boot_cpu = &host_topology.cpus[0];
	vayu_cpu_state[0] = VAYU_CPU_RUNNING;
}

static void
start_cpus(void)
{
	for (unsigned int i = 1; i < host_topology.num_cpus; i++) {
		int32_t rc = vayu_psci_cpu_on(i, host_topology.cpus[i].phys_id, 0x1000, i);
		CHECK(rc == fw_cpu_on_rc[i]);
		if (rc == 0 && cpu_mode[i] != CPU_NO_THREAD) {
			pthread_create(&threads[i], NULL, cpu_thread, (void *)(intptr_t)i);
		} else {
			threads[i] = 0;
		}
	}
	/* wait until the started ones are in "the kernel" */
	for (unsigned int i = 1; i < host_topology.num_cpus; i++) {
		while (threads[i] && os_atomic_load(&vayu_cpu_state[i], acquire) == VAYU_CPU_ASKED) {
			usleep(100);
		}
	}
}

static void
finish(void)
{
	quit = 1;
	for (unsigned int i = 1; i < host_topology.num_cpus; i++) {
		if (threads[i]) {
			pthread_join(threads[i], NULL);
		}
	}
	memset(cpu_mode, 0, sizeof(cpu_mode));
	memset(fw_cpu_on_rc, 0, sizeof(fw_cpu_on_rc));
	vayu_smp_arrive_ns = 1000ULL * 1000 * 1000;
}

static bool
all_others_off(void)
{
	for (unsigned int i = 1; i < host_topology.num_cpus; i++) {
		if (vayu_cpu_state[i] == VAYU_CPU_ABANDONED) {
			continue;       /* never ran kernel code (the firmware may still say ON): its gate is closed */
		}
		/* off as the stop confirmed it, or never ran (CPU_ON refused) */
		if ((vayu_cpu_state[i] != VAYU_CPU_OFF && vayu_cpu_state[i] != VAYU_CPU_NONE) || fw_power[i]) {
			return false;
		}
	}
	return true;
}

/* the return from the boot CPU (this thread) */
static void
boot_returns(void)
{
	host_cpu = 0;
	loader_return();
}

static void *
boot_thread(void *arg)
{
	(void)arg;
	host_cpu = 0;
	while (!quit && !returned_jump && !returned_fail) {
		if (sgi2_pending[0]) {
			sgi2_pending[0] = 0;
			vayu_smp_sgi_stop();
		}
		vayu_smp_poll();
		usleep(50);
	}
	return NULL;
}

static void
run(const char *name, bool want_back, void (*arrange)(void), void (*go)(void))
{
	setup(7);
	if (arrange) {
		arrange();
	}
	start_cpus();
	uint64_t t0 = mach_absolute_time();
	go();
	uint64_t ms = (mach_absolute_time() - t0) / 1000000;
	bool back = returned_jump && !returned_fail;
	printf("%-13s %s (%s expected), from cpu %d, %llu ms, states:", name, back ? "BACK" : "NOT BACK",
	    want_back ? "BACK" : "NOT BACK", returned_cpu, (unsigned long long)ms);
	for (unsigned int i = 0; i < host_topology.num_cpus; i++) {
		printf(" %s", vayu_cpu_state_name(vayu_cpu_state[i]));
	}
	printf("\n");
	CHECK(back == want_back);
	CHECK(returned_cpu == 0);               /* the boot CPU gives the machine back, whoever began */
	if (want_back) {
		CHECK(all_others_off());
	}
	finish();
}

static void go_boot(void) { boot_returns(); }

void *
handover_cpu3(void *arg)
{
	(void)arg;
	host_cpu = 3;
	vayu_smp_stop_others();                 /* does not come back: hands over, then CPU_OFF ends this thread */
	return NULL;
}

static void
go_handover(void)
{
	/* CPU 3 begins the way back (loader_return_after's thread ran there, or it panicked): it hands over and goes off */
	pthread_t b, r;
	pthread_create(&b, NULL, boot_thread, NULL);
	cpu_mode[3] = CPU_DEAF;                 /* CPU 3's idle thread steps aside: CPU 3 is the one returning */
	pthread_create(&r, NULL, handover_cpu3, NULL);
	pthread_join(r, NULL);
	while (!returned_jump && !returned_fail) {
		usleep(100);
	}
	pthread_join(b, NULL);
}

static void arrange_deaf(void) { cpu_mode[5] = CPU_DEAF; }
static void arrange_offrefused(void) { cpu_mode[2] = CPU_OFF_REFUSED; }
static void arrange_neverarrived(void) { cpu_mode[6] = CPU_NO_THREAD; }
static void arrange_onrefused(void) { fw_cpu_on_rc[4] = VAYU_PSCI_DENIED; }
static void arrange_alreadyon(void) { fw_cpu_on_rc[4] = VAYU_PSCI_ALREADY_ON; cpu_mode[4] = CPU_NO_THREAD; }

static void
go_wentoff(void)
{
	fw_power[6] = 0;                        /* the never-arrived CPU is off as the firmware sees it */
	boot_returns();
}

static void
go_noaffinity(void)
{
	fw_affinity_unsupported = 1;
	boot_returns();
}

/* ---- K2b: the arrival watch ---- */

static int arrival_seen;

/* SweetCPU::startAll's loop for CPU 6 (1 ms a pass), for at most `ms`: what vayu_smp_arrival() said last */
static int
watch6(unsigned int ms)
{
	int a = VAYU_ARRIVAL_WAIT;
	for (unsigned int t = 0; t < ms && a == VAYU_ARRIVAL_WAIT; t++) {
		a = vayu_smp_arrival(6);
		usleep(1000);
	}
	return a;
}

static void arrange_abandoned(void) { cpu_mode[6] = CPU_NO_THREAD; vayu_smp_arrive_ns = 20ULL * 1000 * 1000; }

static void
go_abandoned(void)
{
	arrival_seen = watch6(500);
	CHECK(arrival_seen == VAYU_ARRIVAL_NEVER);
	CHECK(vayu_smp_slot[6].gate[0] == 1);
	CHECK(vayu_cpu_state[6] == VAYU_CPU_ABANDONED);
	boot_returns();
}

/* the CPU's first instructions (start.s sweet_secondary_entry) after the verdict: checkpoint 1, then the gate */
static void
go_lategate(void)
{
	go_abandoned();
	volatile struct vayu_smp_slot *s = &vayu_smp_slot[6];
	s->early[0] = VAYU_CRUMB(VAYU_CRUMB_ENTRY);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	CHECK(s->gate[0] != 0);                 /* closed: it switches itself off */
	s->early[0] = VAYU_CRUMB(VAYU_CRUMB_ABANDONED);
	fw_power[6] = 0;
	CHECK(vayu_smp_arrival(6) == VAYU_ARRIVAL_GONE || vayu_cpu_state[6] == VAYU_CPU_ABANDONED);
}

static void
arrange_stuckearly(void)
{
	cpu_mode[6] = CPU_NO_THREAD;
	vayu_smp_arrive_ns = 20ULL * 1000 * 1000;
}

static void
go_stuckearly(void)
{
	vayu_smp_slot[6].early[0] = VAYU_CRUMB(VAYU_CRUMB_CPUDATA);     /* it reached reset_vector, then nothing */
	arrival_seen = watch6(60);
	CHECK(arrival_seen == VAYU_ARRIVAL_WAIT);
	CHECK(vayu_smp_slot[6].gate[0] == 0);   /* never closed on a CPU that has started */
	CHECK(vayu_cpu_state[6] == VAYU_CPU_ASKED);
	boot_returns();
}

static void
go_earlyfault(void)
{
	volatile struct vayu_smp_slot *s = &vayu_smp_slot[6];
	s->early[4] = 0x62000000;               /* ESR: EC 0x18, an MSR trapped */
	s->early[0] = VAYU_CRUMB(VAYU_CRUMB_FAULT | VAYU_CRUMB_EL1);
	fw_power[6] = 0;                        /* its CPU_OFF */
	arrival_seen = watch6(60);
	CHECK(arrival_seen == VAYU_ARRIVAL_GONE);
	boot_returns();
}

static void
go_parked(void)
{
	vayu_smp_slot[6].early[0] = VAYU_CRUMB(VAYU_CRUMB_OFF_REFUSED);
	arrival_seen = watch6(60);
	CHECK(arrival_seen == VAYU_ARRIVAL_PARKED);
	boot_returns();
}

static void
single(void)
{
	setup(7);
	host_cpu = 0;
	CHECK(vayu_smp_stop_others());
	printf("%-13s BACK (BACK expected): no CPU was ever asked\n", "single");
}

int
main(void)
{
	single();
	run("good", true, NULL, go_boot);
	run("handover", true, NULL, go_handover);
	run("deaf", false, arrange_deaf, go_boot);
	run("offrefused", false, arrange_offrefused, go_boot);
	run("neverarrived", false, arrange_neverarrived, go_boot);
	run("wentoff", true, arrange_neverarrived, go_wentoff);
	run("noaffinity", false, NULL, go_noaffinity);
	run("onrefused", true, arrange_onrefused, go_boot);
	run("alreadyon", false, arrange_alreadyon, go_boot);
	run("abandoned", true, arrange_abandoned, go_abandoned);
	run("lategate", true, arrange_abandoned, go_lategate);
	run("stuckearly", false, arrange_stuckearly, go_stuckearly);
	run("earlyfault", true, arrange_neverarrived, go_earlyfault);
	run("parked", false, arrange_neverarrived, go_parked);
	printf("psci_host_test: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
