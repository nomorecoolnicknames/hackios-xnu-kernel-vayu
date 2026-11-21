/*
 * The world sensors.c and motion.c see in the tests: a clock that only moves when the code sleeps,
 * /dev nodes that appear, fail and come back when a scenario says, and the HID event server as a
 * recorder of what was made and sent. Included by the test after the code under test.
 */
#define S                       1000000000ULL   /* ns */
#define MS                      1000000ULL
#define NEVER                   UINT64_MAX

/* ---- the clock --------------------------------------------------------------------------- */

static uint64_t fake_now;               /* ns since boot */
static uint64_t fake_deadline;          /* the scenario ends here (longjmp out of the endless loop) */
static jmp_buf fake_end;

static void
fake_advance(uint64_t ns)
{
	fake_now += ns;
	if (fake_now >= fake_deadline) {
		longjmp(fake_end, 1);
	}
}

uint64_t mach_absolute_time(void) { return fake_now; }
int mach_timebase_info(mach_timebase_info_data_t *info) { info->numer = info->denom = 1; return 0; }
unsigned fake_sleep(unsigned seconds) { fake_advance(seconds * S); return 0; }
int fake_usleep(useconds_t us) { fake_advance((uint64_t)us * 1000); return 0; }

/* ---- the devices ------------------------------------------------------------------------- */

enum { NODE_ALS, NODE_PROX, NODE_MOTION_V1 };

struct sample {
	uint64_t at;            /* when the sensor core has it */
	int32_t value;          /* lux_milli; near (1, 0, -1 unknown); accel x */
};

struct node {
	const char *path;
	int kind;
	uint64_t appear;        /* there from then on (NEVER: not in this boot) */
	uint64_t fail_from, fail_to;    /* gone in [from, to): reads fail with EIO, opens with ENOENT */
	const struct sample *samples;
	unsigned count;
	unsigned opens, open_tries;
};

static struct node *nodes;
static unsigned node_count;
static int fd_node[64];                 /* fd - 100 -> node index, -1 free */

static bool
node_there(const struct node *n)
{
	return fake_now >= n->appear && !(fake_now >= n->fail_from && fake_now < n->fail_to);
}

static struct node *
node_of(const char *path)
{
	for (unsigned i = 0; i < node_count; i++) {
		if (strcmp(nodes[i].path, path) == 0) {
			return &nodes[i];
		}
	}
	return NULL;
}

int
fake_open(const char *path, __attribute__((unused)) int flags, ...)
{
	struct node *n = node_of(path);

	if (n != NULL) {
		n->open_tries++;
	}
	if (n == NULL || !node_there(n)) {
		errno = ENOENT;
		return -1;
	}
	for (int i = 0; i < 64; i++) {
		if (fd_node[i] < 0) {
			fd_node[i] = (int)(n - nodes);
			n->opens++;
			return 100 + i;
		}
	}
	errno = EMFILE;
	return -1;
}

int
fake_access(const char *path, __attribute__((unused)) int mode)
{
	struct node *n = node_of(path);

	if (n == NULL || !node_there(n)) {
		errno = ENOENT;
		return -1;
	}
	return 0;
}

int
fake_close(int fd)
{
	if (fd >= 100 && fd < 164) {
		fd_node[fd - 100] = -1;
	}
	return 0;
}

/* What the kernel's read gives now: seq = the samples so far, the latest one's value (vayu_ssc.c). */
ssize_t
fake_read(int fd, void *buffer, size_t size)
{
	struct node *n;
	uint32_t seq = 0;
	int32_t value = 0;

	if (fd < 100 || fd >= 164 || fd_node[fd - 100] < 0) {
		errno = EBADF;
		return -1;
	}
	n = &nodes[fd_node[fd - 100]];
	if (!node_there(n)) {
		errno = EIO;
		return -1;
	}
	for (unsigned i = 0; i < n->count && n->samples[i].at <= fake_now; i++) {
		seq = i + 1;
		value = n->samples[i].value;
	}
	if (n->kind == NODE_ALS) {
		uint32_t a[4] = { 1, seq, (uint32_t)value, 0 };

		if (size < sizeof(a)) { errno = EINVAL; return -1; }
		memcpy(buffer, a, sizeof(a));
		return sizeof(a);
	}
	if (n->kind == NODE_PROX) {
		int32_t p[5] = { 1, (int32_t)seq, seq ? value : -1, 0, 0 };

		if (size < sizeof(p)) { errno = EINVAL; return -1; }
		memcpy(buffer, p, sizeof(p));
		return sizeof(p);
	}
	{
		int32_t m[6] = { 1, (int32_t)seq, value, 0, 9807, 0 };    /* v1: version 1 at the v1 size */

		if (size < sizeof(m)) { errno = EINVAL; return -1; }
		memcpy(buffer, m, sizeof(m));
		return sizeof(m);
	}
}

static void
nodes_use(struct node *table, unsigned count)
{
	nodes = table;
	node_count = count;
	for (int i = 0; i < 64; i++) {
		fd_node[i] = -1;
	}
}

/* ---- the log ----------------------------------------------------------------------------- */

static char fake_log[1 << 20];
static size_t fake_log_used;

ssize_t
fake_write(int fd, const void *buffer, size_t size)
{
	if (fd == 2 && fake_log_used + size < sizeof(fake_log)) {
		memcpy(fake_log + fake_log_used, buffer, size);
		fake_log_used += size;
		fake_log[fake_log_used] = 0;
		if (getenv("VERBOSE") != NULL) {
			fprintf(stderr, "[%8.3f] %.*s", fake_now / 1e9, (int)size, (const char *)buffer);
		}
	}
	return (ssize_t)size;
}

static unsigned
log_count(const char *what)
{
	unsigned n = 0;

	for (const char *p = fake_log; (p = strstr(p, what)) != NULL; p++) {
		n++;
	}
	return n;
}

/* ---- CoreFoundation, as little as can run ------------------------------------------------ */

static const int cf_true = 1, cf_false = 0, cf_object;
const CFBooleanRef kCFBooleanTrue = (CFBooleanRef)&cf_true, kCFBooleanFalse = (CFBooleanRef)&cf_false;
const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
const CFArrayCallBacks kCFTypeArrayCallBacks;

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex c, const CFDictionaryKeyCallBacks *k,
    const CFDictionaryValueCallBacks *v) { (void)a; (void)c; (void)k; (void)v; return (CFMutableDictionaryRef)&cf_object; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) { (void)d; (void)k; (void)v; }
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { (void)d; (void)k; return NULL; }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { (void)d; return 0; }
void CFDictionaryGetKeysAndValues(CFDictionaryRef d, const void **k, const void **v) { (void)d; (void)k; (void)v; }
CFArrayRef CFArrayCreate(CFAllocatorRef a, const void **v, CFIndex n, const CFArrayCallBacks *c) { (void)a; (void)v; (void)n; (void)c; return (CFArrayRef)&cf_object; }
CFStringRef CFStringCreateWithCString(CFAllocatorRef a, const char *s, CFStringEncoding e) { (void)a; (void)e; return (CFStringRef)s; }
Boolean CFStringGetCString(CFStringRef s, char *b, CFIndex n, CFStringEncoding e) { (void)e; snprintf(b, (size_t)n, "%s", (const char *)s); return 1; }
CFNumberRef CFNumberCreate(CFAllocatorRef a, CFNumberType t, const void *v) { (void)a; (void)t; (void)v; return (CFNumberRef)&cf_object; }
Boolean CFNumberGetValue(CFNumberRef n, CFNumberType t, void *v) { (void)n; (void)t; (void)v; return 0; }
CFTypeRef CFRetain(CFTypeRef o) { return o; }
void CFRelease(CFTypeRef o) { (void)o; }
Boolean CFEqual(CFTypeRef a, CFTypeRef b) { return a == b; }
CFTypeID CFGetTypeID(CFTypeRef o) { (void)o; return 0; }
CFTypeID CFBooleanGetTypeID(void) { return 1; }
CFTypeID CFNumberGetTypeID(void) { return 2; }
Boolean CFBooleanGetValue(CFBooleanRef b) { return b == kCFBooleanTrue; }
CFStringRef CFCopyDescription(CFTypeRef o) { (void)o; return NULL; }
dispatch_queue_t dispatch_queue_create(const char *l, void *a) { (void)l; (void)a; return (dispatch_queue_t)&cf_object; }
int sysctlbyname(const char *n, void *o, size_t *ol, void *nw, size_t nl) { (void)n; (void)o; (void)ol; (void)nw; (void)nl; errno = ENOENT; return -1; }
const char *getprogname(void) { return "host-test"; }

/* ---- the HID event server, as a recorder -------------------------------------------------- */

struct __IOHIDEvent {
	uint32_t type;
	double value;
	uint64_t at;
};

struct dispatched {
	IOHIDServiceClientRef service;
	uint32_t type;
	double value;
	uint64_t at;
};

static struct __IOHIDEvent event_pool[1 << 16];
static unsigned events_made;
static struct dispatched sent[1 << 16];
static unsigned sent_count, clients_made, services_made;
static void *service_refcon[16];
static uint64_t service_at[16];

static void
hid_reset(void)
{
	events_made = sent_count = clients_made = services_made = 0;
	fake_log_used = 0;
	fake_log[0] = 0;
}

static IOHIDEventRef
event_make(uint32_t type, double value)
{
	IOHIDEventRef e = &event_pool[events_made++ % (1 << 16)];

	e->type = type;
	e->value = value;
	e->at = fake_now;
	return e;
}

IOHIDEventSystemClientRef
IOHIDEventSystemClientCreateWithType(CFAllocatorRef a, int type, CFDictionaryRef attributes)
{
	(void)a; (void)type; (void)attributes;
	clients_made++;
	return (IOHIDEventSystemClientRef)&cf_object;
}

void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef c, dispatch_queue_t q) { (void)c; (void)q; }

/* The event server opens every service at once (the code's struct service: opened is set). */
IOHIDServiceClientRef
IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon)
{
	(void)client; (void)attributes; (void)callbacks; (void)target;
	((struct service *)refcon)->opened = true;
	service_refcon[services_made] = refcon;
	service_at[services_made] = fake_now;
	return (IOHIDServiceClientRef)&service_refcon[services_made++];
}

Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef s) { (void)s; return 1; }

Boolean
IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event)
{
	if (sent_count < (1 << 16)) {
		sent[sent_count++] = (struct dispatched){ service, event->type, event->value, fake_now };
	}
	return 1;
}

void IOHIDEventSetIntegerValue(IOHIDEventRef e, uint32_t f, CFIndex v) { (void)e; (void)f; (void)v; }
