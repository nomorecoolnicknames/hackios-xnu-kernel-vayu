/*
 * devices/poco-x3-pro/userland/src/sensors.c on the host: its own thread and loop, run against fake
 * /dev nodes and a fake clock (fake.h), scenario by scenario. What is checked is what the phone
 * would see: whether the services get made, and which ALS/proximity events reach the event server.
 */
#include SENSORS_C
#include "fake.h"

IOHIDEventRef IOHIDEventCreateAmbientLightSensorEvent(CFAllocatorRef a, uint64_t t, uint32_t lux, uint32_t o)
{ (void)a; (void)t; (void)o; return event_make(EVENT_TYPE_ALS, lux); }
IOHIDEventRef IOHIDEventCreateProximtyEvent(CFAllocatorRef a, uint64_t t, uint32_t mask, uint32_t o)
{ (void)a; (void)t; (void)o; return event_make(EVENT_TYPE_PROXIMITY, mask); }

static int failures;

#define CHECK(what, ...) do { if (!(what)) { failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #what); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Run sensors_thread (ALS and proximity wanted) from boot until `until'. */
static void
boot(struct node *table, unsigned count, uint64_t until)
{
	nodes_use(table, count);
	hid_reset();
	memset(&run, 0, sizeof(run));
	fake_now = 0;
	fake_deadline = until;
	if (setjmp(fake_end) == 0) {
		sensors_thread((void *)(intptr_t)3);
		printf("  (the thread returned at %.3f s)\n", fake_now / 1e9);
	}
}

static IOHIDServiceClientRef
service_for(void *refcon)
{
	for (unsigned i = 0; i < services_made; i++) {
		if (service_refcon[i] == refcon) {
			return (IOHIDServiceClientRef)&service_refcon[i];
		}
	}
	return NULL;
}

static unsigned
events(uint32_t type, const struct dispatched **last)
{
	unsigned n = 0;

	*last = NULL;
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].type == type) {
			n++;
			*last = &sent[i];
		}
	}
	return n;
}

/* SLPI off (the default boot): nothing is ever there. Nothing made, said once, looked for sparingly. */
static void
no_sensor_core(void)
{
	struct node table[] = {
		{ "/dev/vayu-als", NODE_ALS, NEVER, 0, 0, NULL, 0, 0, 0 },
		{ "/dev/vayu-prox", NODE_PROX, NEVER, 0, 0, NULL, 0, 0, 0 },
		{ "/dev/vayu-motion", NODE_MOTION_V1, NEVER, 0, 0, NULL, 0, 0, 0 },
	};

	printf("no sensor core (vayu_slpi off), 30 min\n");
	boot(table, 3, 30 * 60 * S);
	CHECK(clients_made == 0, "%u HID clients", clients_made);
	CHECK(services_made == 0, "%u services", services_made);
	CHECK(sent_count == 0, "%u events", sent_count);
	CHECK(log_count("no ambient-light source") == 1, "said %u times", log_count("no ambient-light source"));
	CHECK(log_count("no proximity source") == 1, "said %u times", log_count("no proximity source"));
	/* 2+4+8+16 s, then every 30 s: about 60 looks each in 30 min, not one every tick */
	CHECK(table[0].open_tries <= 70, "%u looks for /dev/vayu-als", table[0].open_tries);
	CHECK(table[1].open_tries <= 70, "%u looks for /dev/vayu-prox", table[1].open_tries);
}

/* The sensor core comes late, one device after the other: each is picked up on its own. */
static void
late_nodes(void)
{
	static const struct sample light[] = { { 61 * S, 120000 }, { 200 * S, 80000 } };
	static const struct sample prox[] = { { 140 * S, 1 } };
	struct node table[] = {
		{ "/dev/vayu-als", NODE_ALS, 60 * S, 0, 0, light, 2, 0, 0 },
		{ "/dev/vayu-prox", NODE_PROX, 130 * S, 0, 0, prox, 1, 0, 0 },
		{ "/dev/vayu-motion", NODE_MOTION_V1, 60 * S, 0, 0, NULL, 0, 0, 0 },
	};
	const struct dispatched *last;
	unsigned n;

	printf("ALS at 60 s, proximity at 130 s (the old code looked once, at 12 s)\n");
	boot(table, 3, 240 * S);
	CHECK(clients_made == 1, "%u HID clients", clients_made);
	CHECK(service_for(&run.als) != NULL && service_for(&run.prox) != NULL, "%u services", services_made);
	CHECK(services_made == 2 && service_at[0] <= (60 + 30) * S, "ALS service at %.3f s", service_at[0] / 1e9);
	CHECK(services_made == 2 && service_at[1] >= 130 * S && service_at[1] <= (130 + 30) * S,
	    "proximity service at %.3f s", service_at[1] / 1e9);
	n = events(EVENT_TYPE_ALS, &last);
	CHECK(n == 2 && last->value == 80 && last->at < 201 * S, "%u ALS events, last %.0f lux at %.3f s", n,
	    last ? last->value : -1, last ? last->at / 1e9 : -1);
	n = events(EVENT_TYPE_PROXIMITY, &last);
	CHECK(n == 1 && last->value == 0x40 && last->at < 141 * S, "%u proximity events", n);
}

/* One change inside the throttling interval, and nothing after it: sent late, not dropped. */
static void
als_last_change(void)
{
	static const struct sample light[] = { { 20 * S, 100000 }, { 20 * S + 50 * MS, 500000 } };
	struct node table[] = {
		{ "/dev/vayu-als", NODE_ALS, 0, 0, 0, light, 2, 0, 0 },
		{ "/dev/vayu-prox", NODE_PROX, NEVER, 0, 0, NULL, 0, 0, 0 },
	};
	const struct dispatched *last;
	unsigned n;

	printf("ALS 100 lux, then 500 lux 50 ms later and no more samples\n");
	boot(table, 2, 30 * S);
	n = events(EVENT_TYPE_ALS, &last);
	CHECK(n == 2, "%u ALS events", n);
	CHECK(last != NULL && last->value == 500, "the last ALS event says %.0f lux", last ? last->value : -1);
	/* the default interval is 200 ms after the first one went (20.00 s) */
	CHECK(last != NULL && last->at >= 20 * S + 200 * MS && last->at <= 20 * S + 260 * MS, "sent at %.3f s",
	    last ? last->at / 1e9 : -1);
}

/* Proximity near, then its device fails: far is said, and near again once it is back. */
static void
prox_lost_while_near(void)
{
	static const struct sample prox[] = { { 20 * S, 1 }, { 70 * S, 1 } };
	struct node table[] = {
		{ "/dev/vayu-als", NODE_ALS, NEVER, 0, 0, NULL, 0, 0, 0 },
		{ "/dev/vayu-prox", NODE_PROX, 0, 30 * S, 45 * S, prox, 2, 0, 0 },
	};
	unsigned seen = 0, near_at_20 = 0, far_at_30 = 0, near_again = 0;

	printf("proximity near at 20 s, its device gone 30-45 s, near again at 70 s\n");
	boot(table, 2, 120 * S);
	for (unsigned i = 0; i < sent_count; i++) {
		if (sent[i].type != EVENT_TYPE_PROXIMITY) continue;
		seen++;
		if (sent[i].value == 0x40 && sent[i].at >= 20 * S && sent[i].at < 21 * S) near_at_20++;
		if (sent[i].value == 0 && sent[i].at >= 30 * S && sent[i].at < 30 * S + 100 * MS) far_at_30++;
		if (sent[i].value == 0x40 && sent[i].at >= 45 * S && sent[i].at < 80 * S) near_again++;
	}
	CHECK(near_at_20 == 1, "near at 20 s: %u", near_at_20);
	CHECK(far_at_30 == 1, "far when the device failed: %u", far_at_30);
	CHECK(near_again == 1, "near again after it came back: %u", near_again);
	CHECK(seen == 3, "%u proximity events", seen);
	CHECK(table[1].opens == 2, "/dev/vayu-prox opened %u times", table[1].opens);
}

/* A kernel that says "unknown" (near -1) after near: far, not stuck near. */
static void
prox_unknown_while_near(void)
{
	static const struct sample prox[] = { { 20 * S, 1 }, { 25 * S, -1 } };
	struct node table[] = {
		{ "/dev/vayu-als", NODE_ALS, NEVER, 0, 0, NULL, 0, 0, 0 },
		{ "/dev/vayu-prox", NODE_PROX, 0, 0, 0, prox, 2, 0, 0 },
	};
	const struct dispatched *last;
	unsigned n;

	printf("proximity near at 20 s, unknown at 25 s\n");
	boot(table, 2, 40 * S);
	n = events(EVENT_TYPE_PROXIMITY, &last);
	CHECK(n == 2 && last->value == 0 && last->at >= 25 * S && last->at < 25 * S + 100 * MS,
	    "%u proximity events, the last %#x at %.3f s", n, last ? (unsigned)last->value : 0, last ? last->at / 1e9 : -1);
}

int
main(void)
{
	no_sensor_core();
	late_nodes();
	als_last_change();
	prox_lost_while_near();
	prox_unknown_while_near();
	printf("sensors-test: %s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
	return failures != 0;
}
