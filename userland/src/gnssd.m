/*
 * gnssd: the modem's GPS fixes, from the kernel to locationd (LOCATIOND=1 in build.sh).
 *
 * The kernel asks the modem's location service for a fix (vayu_gnss=1, bsd/dev/vayu_gnss.c; with
 * vayu_gnss_run=0 for as long as the modem runs) and keeps the latest in a 96-byte record, read
 * from /dev/vayu-gnss (sys/vayu_gnss.h: magic 'GNSS', version 1). locationd has no GPS of its own
 * to read here -- iOS's receiver is behind a driver this phone has not got -- but what Xcode's
 * "simulate location" uses is open to a program with the entitlement com.apple.locationd.simulation
 * (gnssd.entitlements): CoreLocation's private CLSimulationManager hands locationd locations that it
 * then gives every application as its own. Each new fix replaces the one before: stopped, cleared,
 * appended, flushed, started. That is xreactx/oneplus-6t-upstream 12d310ec3's userland/src/gnssd.m,
 * which gave Compass the 6T's fix; here it reads the record instead of that port's sysctls.
 *
 * A fix is handed on only when the record says it is one: FLAG_POSITION set (the kernel clears it
 * when the position is more than 5 s old) and a sequence number not seen yet. Its accuracy is the
 * engine's horizontal error when the engine gave one (the v02 report, vayu_gnss_v02=1), else the
 * HDOP times 5 m -- an estimate, a usual figure for a receiver's range error, said as such in the
 * log -- else 50 m. Its time is now, or the fix's own UTC time of day when that is within 30 s of
 * the system's clock (the loader sets it from the RTC: epoch=).
 *
 * Without vayu_gnss=1 among the boot-args it ends at once; with it, it waits for /dev/vayu-gnss,
 * which the kernel makes when the modem's link is up. Lines on the console: the first fix, then one
 * in 60, the position rounded to 0.01 degree (about a kilometre: the log travels).
 *
 * From another HakiOS port (MIT, as this file): xreactx/hakios-xnu-kernel, branch oneplus-6t-upstream, commit 12d310ec3b2e
 * (its userland/src/gnssd.m).
 */
#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

@interface CLSimulationManager : NSObject
@property (assign, nonatomic) uint8_t locationRepeatBehavior;
- (void)clearSimulatedLocations;
- (void)appendSimulatedLocation:(CLLocation *)location;
- (void)flush;
- (void)startLocationSimulation;
- (void)stopLocationSimulation;
@end

/* sys/vayu_gnss.h of the kernel, as it is there (version 1) */
#define GNSS_MAGIC              0x53534e47u
#define GNSS_VERSION            1
#define GNSS_POSITION           0x00000001u
#define GNSS_ALTITUDE           0x00000002u
#define GNSS_HEPE               0x00000004u
#define GNSS_HDOP               0x00000008u
#define GNSS_SPEED              0x00000010u
#define GNSS_HEADING            0x00000020u
#define GNSS_TIME_OF_DAY        0x00000040u
#define GNSS_STALE              0x00000200u
#define GNSS_FAILED             4

struct gnss_record {
	uint32_t magic, version, size, seq;
	uint64_t updated_ms, started_ms;
	int32_t lat_e7, lon_e7, altitude_mm, hepe_cm;
	uint32_t hdop_centi, speed_mm_s, heading_mdeg, tod_ms, ttff_ms, fixes, nmea, sv_reports;
	uint16_t sats_used, sats_view;
	uint8_t source, quality, session, online;
	uint32_t flags, reserved;
};

_Static_assert(sizeof(struct gnss_record) == 96 && offsetof(struct gnss_record, lat_e7) == 32 &&
    offsetof(struct gnss_record, sats_used) == 80 && offsetof(struct gnss_record, flags) == 88,
    "the record of sys/vayu_gnss.h");

static bool
gnss_asked(void)
{
	static char args[8192];                 /* (this port's command lines are well past 1 KB) */
	size_t length = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &length, NULL, 0) != 0) {
		return false;
	}
	args[length < sizeof(args) ? length : sizeof(args) - 1] = 0;
	for (const char *at = args; (at = strstr(at, "vayu_gnss=")) != NULL; at++) {
		if ((at == args || at[-1] == ' ') && at[10] != '0') {
			return true;
		}
	}
	return false;
}

/* When the fix was: its own UTC time of day on today's date if that is within 30 s of now, else now. */
static NSDate *
fix_time(const struct gnss_record *record)
{
	time_t now = time(NULL);
	long long midnight = (long long)now - (long long)now % 86400, at;

	if (!(record->flags & GNSS_TIME_OF_DAY) || record->tod_ms >= 86400000u || now < 1700000000) {
		return [NSDate date];
	}
	at = midnight * 1000 + record->tod_ms;
	if (at - (long long)now * 1000 > 43200000LL) {
		at -= 86400000LL;               /* (just after midnight: the fix was yesterday's) */
	} else if ((long long)now * 1000 - at > 43200000LL) {
		at += 86400000LL;
	}
	if (llabs(at - (long long)now * 1000) > 30000) {
		return [NSDate date];
	}
	return [NSDate dateWithTimeIntervalSince1970:at / 1000.0];
}

int
main(void)
{
	@autoreleasepool {
		struct gnss_record record;
		CLSimulationManager *simulation;
		uint32_t seen = 0;
		unsigned long long given = 0;
		int fd = -1, wrong = 0;

		setvbuf(stdout, NULL, _IOLBF, 0);
		if (!gnss_asked()) {
			printf("gnssd: no vayu_gnss=1 among the boot-args: nothing to do\n");
			return 0;
		}
		simulation = [[NSClassFromString(@"CLSimulationManager") alloc] init];
		if (simulation == nil) {
			printf("gnssd: no CLSimulationManager in CoreLocation\n");
			return 1;
		}
		simulation.locationRepeatBehavior = 0;
		printf("gnssd: waiting for /dev/vayu-gnss and the kernel's first fix\n");
		for (;;) {
			double accuracy;
			const char *how;
			CLLocation *location;

			if (fd < 0 && (fd = open("/dev/vayu-gnss", O_RDONLY)) < 0) {
				sleep(5);
				continue;
			}
			/* (the driver hands the whole record to every read, whatever the offset; a node just made may
			 * not have its first record yet, so a wrong one is only final after ten seconds of them) */
			if (read(fd, &record, sizeof(record)) != (ssize_t)sizeof(record) || record.magic != GNSS_MAGIC ||
			    record.version != GNSS_VERSION || record.size != sizeof(record)) {
				if (++wrong >= 10) {
					printf("gnssd: /dev/vayu-gnss does not give a version-1 record (%s): stopping\n", strerror(errno));
					return 1;
				}
				sleep(1);
				continue;
			}
			wrong = 0;
			if (record.seq == seen || !(record.flags & GNSS_POSITION) || (record.flags & GNSS_STALE)) {
				usleep(record.session == GNSS_FAILED ? 5000000 : 250000);
				continue;
			}
			seen = record.seq;
			if ((record.flags & GNSS_HEPE) && record.hepe_cm > 0) {
				accuracy = record.hepe_cm / 100.0;
				how = "the engine's error";
			} else if ((record.flags & GNSS_HDOP) && record.hdop_centi != 0xffffffffu && record.hdop_centi != 0) {
				accuracy = record.hdop_centi / 100.0 * 5.0;
				how = "HDOP x 5 m, an estimate";
			} else {
				accuracy = 50.0;
				how = "unknown: 50 m";
			}
			location = [[CLLocation alloc] initWithCoordinate:CLLocationCoordinate2DMake(record.lat_e7 / 1e7, record.lon_e7 / 1e7)
			    altitude:(record.flags & GNSS_ALTITUDE) ? record.altitude_mm / 1000.0 : 0
			    horizontalAccuracy:accuracy verticalAccuracy:(record.flags & GNSS_ALTITUDE) ? accuracy * 1.5 : -1
			    course:(record.flags & GNSS_HEADING) ? record.heading_mdeg / 1000.0 : -1
			    speed:(record.flags & GNSS_SPEED) ? record.speed_mm_s / 1000.0 : -1 timestamp:fix_time(&record)];
			[simulation stopLocationSimulation];
			[simulation clearSimulatedLocations];
			[simulation appendSimulatedLocation:location];
			[simulation flush];
			[simulation startLocationSimulation];
			if (given++ % 60 == 0) {
				printf("gnssd: fix %u given to locationd (about %.2f, %.2f; within %.0f m: %s; %u satellites used)\n",
				    record.seq, record.lat_e7 / 1e7, record.lon_e7 / 1e7, accuracy, how,
				    record.sats_used != 0xffff ? record.sats_used : 0);
			}
		}
	}
}
