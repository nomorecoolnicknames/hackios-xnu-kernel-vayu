/*
 * rmtfs for the POCO X3 Pro (vayu): the EFS block service the modem's firmware asks for over
 * QRTR before it will leave its boot phase (QMI service 14).  See bsd/dev/vayu_rmtfs.c.
 *
 * The server is the last piece of the chain that the modem -- and through it Wi-Fi's WLFW and
 * GNSS -- waits for.  It is read-only with respect to the phone's storage: the four EFS
 * partitions (modemst1, modemst2, fsg, fsc) are read through the kernel's read-only UFS block
 * service, and every write the modem sends is kept in a RAM overlay and thrown away at reboot.
 */
#ifndef _SYS_VAYU_RMTFS_H_
#define _SYS_VAYU_RMTFS_H_

#ifdef KERNEL
#include <stdint.h>

struct vayu_qrtr;

/* Boot-arg vayu_rmtfs=0 (nothing), 1 (publish and log requests, answer nothing), 2 (serve). */
void vayu_rmtfs_prepare(void);   /* early 3 MB buffer, before the heap fragments */
void vayu_rmtfs_start(struct vayu_qrtr *q);
void vayu_rmtfs_stop(void);
#endif

#endif /* _SYS_VAYU_RMTFS_H_ */
