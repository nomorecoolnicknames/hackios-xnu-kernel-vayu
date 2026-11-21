/*
 * devices/poco-x3-pro/userland/src/motion.c on the host, as test_sensors.c does sensors.c: its thread
 * against a fake /dev/vayu-motion and a fake clock.
 */
#include MOTION_C
#include "fake.h"

IOHIDEventRef IOHIDEventCreateAccelerometerEvent(CFAllocatorRef a, uint64_t t, double x, double y, double z, uint32_t o)
{ (void)a; (void)t; (void)y; (void)z; (void)o; return event_make(1, x); }
Class objc_getClass(const char *name) { (void)name; return Nil; }
SEL sel_registerName(const char *name) { return (SEL)name; }
Method class_getClassMethod(Class cls, SEL sel) { (void)cls; (void)sel; return NULL; }
bool class_respondsToSelector(Class cls, SEL sel) { (void)cls; (void)sel; return false; }
Class object_getClass(id obj) { (void)obj; return Nil; }
static void msgsend_unused(void) { }
void (*const objc_msgSend)(void) = msgsend_unused;

static int failures;

#define CHECK(what, ...) do { if (!(what)) { failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #what); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static void
boot(struct node *table, unsigned count, uint64_t until)
{
	nodes_use(table, count);
	hid_reset();
	motion_interval_us = 0;
	fake_now = 0;
	fake_deadline = until;
	if (setjmp(fake_end) == 0) {
		motion_thread((void *)(intptr_t)1);
		printf("  (the thread returned at %.3f s)\n", fake_now / 1e9);
	}
}

/* Samples sent between from and to, and the first one's time. */
static unsigned
sent_between(uint64_t from, uint64_t to, uint64_t *first)
{
	unsigned n = 0;

	*first = 0;
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].at >= from && sent[i].at < to) {
			if (n++ == 0) *first = sent[i].at;
		}
	}
	return n;
}

static struct sample stream[4000];

/* a sample every 100 ms from `from' on */
static unsigned
stream_from(uint64_t from)
{
	for (unsigned i = 0; i < 4000; i++) {
		stream[i] = (struct sample){ from + i * 100 * MS, (int32_t)i };
	}
	return 4000;
}

static void
never(void)
{
	struct node table[] = { { "/dev/vayu-motion", NODE_MOTION_V1, NEVER, 0, 0, NULL, 0, 0, 0 } };

	printf("no sensor core (vayu_slpi off), 30 min\n");
	boot(table, 1, 30 * 60 * S);
	CHECK(clients_made == 0 && services_made == 0 && sent_count == 0, "%u clients, %u services, %u events",
	    clients_made, services_made, sent_count);
	CHECK(log_count("looking again") == 1, "said %u times", log_count("looking again"));
}

/* The device after 6 min: past the old 5-minute limit, still picked up. */
static void
late(void)
{
	unsigned count = stream_from(360 * S);
	struct node table[] = { { "/dev/vayu-motion", NODE_MOTION_V1, 360 * S, 0, 0, stream, count, 0, 0 } };
	uint64_t first;
	unsigned n;

	printf("/dev/vayu-motion at 6 min (the old code gave up at 5)\n");
	boot(table, 1, 420 * S);
	CHECK(clients_made == 1 && services_made == 2, "%u clients, %u services", clients_made, services_made);
	n = sent_between(360 * S, 420 * S, &first);
	/* found within one 30 s wait, then 10 s for the input thread's turn */
	CHECK(n > 100 && first <= (360 + 30 + 10 + 1) * S, "%u events, the first at %.3f s", n, first / 1e9);
}

/* The device fails for 20 s and comes back: the samples go on, the services stay. */
static void
lost_and_back(void)
{
	unsigned count = stream_from(20 * S);
	struct node table[] = { { "/dev/vayu-motion", NODE_MOTION_V1, 0, 60 * S, 80 * S, stream, count, 0, 0 } };
	uint64_t first;
	unsigned before, during, after;

	printf("/dev/vayu-motion fails 60-80 s\n");
	boot(table, 1, 180 * S);
	before = sent_between(20 * S, 60 * S, &first);
	during = sent_between(60 * S + 300 * MS, 80 * S, &first);
	after = sent_between(80 * S, 180 * S, &first);
	CHECK(services_made == 2, "%u services", services_made);
	CHECK(before > 100 && during == 0 && after > 100, "%u / %u / %u events", before, during, after);
	CHECK(first <= (80 + 30) * S, "back at %.3f s", first / 1e9);
	CHECK(table[0].opens >= 2 && table[0].open_tries < 20, "%u opens, %u tries", table[0].opens, table[0].open_tries);
}

int
main(void)
{
	never();
	late();
	lost_and_back();
	printf("motion-test: %s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
