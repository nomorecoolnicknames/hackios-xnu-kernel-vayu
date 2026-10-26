/*
 * GNSS (the modem's QMI LOC service) for the POCO X3 Pro (vayu, SM8150-AC): the fixed record
 * bsd/dev/vayu_gnss.c leaves for a userland shim to read through /dev/vayu-gnss, and the two
 * calls vayu_modem.c makes around the modem's life.
 *
 * What leaves the kernel is a fix: where, how good, and when. It is the record a location client
 * in user space needs (CoreLocation on the port has no chip of its own: the GNSS engine is in the
 * modem), so unlike the log -- which keeps two decimals -- the record keeps the full 1e-7 degrees
 * the engine gave.
 *
 * Boot-args (bsd/dev/vayu_gnss.c has their meaning in full):
 *   vayu_gnss=0             nothing at all (the default)
 *   vayu_gnss=1             one session: wait for DMS ONLINE and the LOC service, register for the
 *                           position/SV-info/NMEA events, LOC_START, log the fixes for
 *                           vayu_gnss_run seconds, LOC_STOP; the record stays readable
 *   vayu_gnss_wait=S        how long to wait for the services and DMS ONLINE (default 30, 1..120)
 *   vayu_gnss_run=S         how long a session runs after LOC_START (default 120, 5..600)
 *   vayu_gnss_interval_ms=N minimum interval between fixes (default 1000, 200..60000)
 *
 * Record layout: `magic` is 'GNSS' little-endian, `version` is VAYU_GNSS_VERSION and `size` is
 * sizeof(struct vayu_gnss_record) (96 bytes); a shim that does not find them should not read the
 * rest. Numbers are the machine's (little-endian on arm64); there are no strings in it.
 */
#ifndef _SYS_VAYU_GNSS_H_
#define _SYS_VAYU_GNSS_H_

#include <stdint.h>

#define VAYU_GNSS_MAGIC          0x53534e47u    /* "GNSS", little-endian: 'G' is the low byte */
#define VAYU_GNSS_VERSION        1

/* What of the record was filled. */
#define VAYU_GNSS_FLAG_POSITION       0x00000001u       /* lat_e7/lon_e7 are a fix */
#define VAYU_GNSS_FLAG_ALTITUDE       0x00000002u
#define VAYU_GNSS_FLAG_HEPE           0x00000004u       /* horizontal estimated position error */
#define VAYU_GNSS_FLAG_HDOP           0x00000008u
#define VAYU_GNSS_FLAG_SPEED          0x00000010u
#define VAYU_GNSS_FLAG_HEADING        0x00000020u
#define VAYU_GNSS_FLAG_TIME_OF_DAY    0x00000040u       /* tod_ms is the fix's UTC time of day */
#define VAYU_GNSS_FLAG_SATS_USED      0x00000080u
#define VAYU_GNSS_FLAG_SATS_VIEW      0x00000100u
#define VAYU_GNSS_FLAG_STALE          0x00000200u       /* the position is more than 5 s old */
#define VAYU_GNSS_FLAG_ONLINE         0x00000400u       /* DMS said the modem is ONLINE */
#define VAYU_GNSS_FLAG_EVENTS         0x00000800u       /* Register Events was accepted */
#define VAYU_GNSS_FLAG_STARTED        0x00001000u       /* LOC_START was accepted */
#define VAYU_GNSS_FLAG_STOPPED        0x00002000u       /* LOC_STOP was accepted */
#define VAYU_GNSS_FLAG_NMEA_SEEN      0x00004000u
#define VAYU_GNSS_FLAG_SV_SEEN        0x00008000u

/* Which reader filled the position: the binary report, the sentences, or both. */
enum {
	VAYU_GNSS_FROM_NONE = 0,
	VAYU_GNSS_FROM_QMI = 1,
	VAYU_GNSS_FROM_NMEA = 2,
	VAYU_GNSS_FROM_BOTH = 3,
};

/* What the client is doing; the log says the same in words. */
enum {
	VAYU_GNSS_IDLE = 0,             /* not started */
	VAYU_GNSS_WAITING = 1,          /* for the services and DMS ONLINE */
	VAYU_GNSS_RUNNING = 2,          /* LOC_START accepted, the session is on */
	VAYU_GNSS_STOPPED = 3,          /* LOC_STOP sent (or the session ran its time) */
	VAYU_GNSS_FAILED = 4,           /* nothing could be started; see the log */
};

struct vayu_gnss_record {
	uint32_t magic;                 /*   0: VAYU_GNSS_MAGIC */
	uint32_t version;               /*   4: VAYU_GNSS_VERSION */
	uint32_t size;                  /*   8: sizeof(struct vayu_gnss_record) */
	uint32_t seq;                   /*  12: raised by one per fix with a position; 0: none yet */
	uint64_t updated_ms;            /*  16: uptime of the last write, milliseconds */
	uint64_t started_ms;            /*  24: uptime of LOC_START; 0: not started */
	int32_t  lat_e7;                /*  32: degrees * 1e7, north positive */
	int32_t  lon_e7;                /*  36: degrees * 1e7, east positive */
	int32_t  altitude_mm;           /*  40: metres * 1000, above the ellipsoid the engine uses */
	int32_t  hepe_cm;               /*  44: horizontal error estimate, centimetres */
	uint32_t hdop_centi;            /*  48: HDOP * 100; 0xffffffff: unknown */
	uint32_t speed_mm_s;            /*  52 */
	uint32_t heading_mdeg;          /*  56: degrees * 1000, 0 is north */
	uint32_t tod_ms;                /*  60: UTC milliseconds of the day of the fix; 0xffffffff: unknown */
	uint32_t ttff_ms;               /*  64: LOC_START to the first fix; 0xffffffff: none yet */
	uint32_t fixes;                 /*  68: fixes with a position in this session */
	uint32_t nmea;                  /*  72: NMEA sentences read */
	uint32_t sv_reports;            /*  76: SV-info indications seen */
	uint16_t sats_used;             /*  80: 0xffff: unknown */
	uint16_t sats_view;             /*  82: 0xffff: unknown */
	uint8_t  source;                /*  84: VAYU_GNSS_FROM_* */
	uint8_t  quality;               /*  85: NMEA GGA fix quality; 0xff: unknown */
	uint8_t  session;               /*  86: VAYU_GNSS_IDLE..FAILED */
	uint8_t  online;                /*  87: the DMS mode last seen; 0xff: unknown */
	uint32_t flags;                 /*  88: VAYU_GNSS_FLAG_* */
	uint32_t reserved;              /*  92: 0 */
};                                      /* 96 bytes: no padding after `reserved` */

#ifdef KERNEL
struct vayu_qrtr;

/* Start after the modem's GLINK channel is up and QRTR is attached (vayu_modem.c step_link). */
void vayu_gnss_start(struct vayu_qrtr *q);
/* Stop, send LOC_STOP while the modem is still there, and wait for the thread; the record stays. */
void vayu_gnss_stop(void);
#endif

#endif /* _SYS_VAYU_GNSS_H_ */
