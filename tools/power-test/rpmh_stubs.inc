static bool leaving, interrupts, quiet, busy;
static int rpmh_tcs_lock;
static const char *rpmh_tcs_holder;
static uint64_t rpmh_tcs_held_since, clock_us;
static struct { uint32_t phys; } rsc = { 0x18220000 };
#define RPMH_TCS 0
static bool ml_get_interrupts_enabled(void) { return interrupts; }
static bool psci_smp_is_quiesced(void) { return quiet; }
static bool lck_mtx_try_lock(int *lock) { (void)lock; return !busy; }
static void lck_mtx_unlock(int *lock) { (void)lock; }
static uint64_t now_us(void) { clock_us += 10000000; return clock_us; }
static void sleep_us(uint32_t us) { (void)us; }
