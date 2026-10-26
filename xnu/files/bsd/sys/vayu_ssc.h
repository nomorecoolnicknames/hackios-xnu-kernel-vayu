/*
 * The sensors' QMI service (SSC, service 0x190) on the sensors DSP of the POCO X3 Pro
 * (vayu, SM8150-AC): bsd/dev/vayu_ssc.c. The service lives on the SLPI; vayu_slpi.c
 * brings the SLPI up and this is what talks to SSC once its GLINK channel is open.
 */
#ifndef _SYS_VAYU_SSC_H_
#define _SYS_VAYU_SSC_H_

#include <stdbool.h>
#include <stdint.h>

struct hakios_qrtr_ops;

/*
 * Talk to SSC at (node, port) through the router `ops' (vayu_pas.c's vayu_qrtr_ops, attached to
 * the SLPI's IPCRTR channel): level 4 = SUID lookups + attributes; level 5 = also stream accel
 * (then gyro) for `secs' seconds each, then disable. One port of ours is opened for the run and
 * closed before returning. True when at least one SUID answer came back.
 */
bool vayu_ssc_run(const struct hakios_qrtr_ops *ops, uint32_t node, uint32_t port, uint32_t level, uint32_t secs);

/* Level 6's endless stream ends (vayu_slpi_quiesce, at the run's end and on a panic). */
void vayu_ssc_quiesce(void);

/*
 * What a read of /dev/vayu-motion gives (level 6): the latest accelerometer sample as SSC reports
 * it, in mm/s^2, x y z in the sensor's frame (no mount matrix applied). seq counts samples; 0: none yet.
 */
#define VAYU_MOTION_VERSION     1
struct vayu_motion {
	uint32_t version;
	uint32_t seq;
	int32_t  accel_mm_s2[3];
	uint32_t age_ms;
};

/* The last accelerometer sample in mm/s^2 (x, y, z as SSC reports them) and its age; false if none yet. */
bool vayu_ssc_accel(int32_t mm_s2[3], uint64_t *age_ns);

#endif /* _SYS_VAYU_SSC_H_ */
