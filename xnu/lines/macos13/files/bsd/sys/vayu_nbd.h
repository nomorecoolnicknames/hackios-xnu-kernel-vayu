/*
 * A disk of the computer at the other end of the USB cable (or of QEMU's host) as /dev/nbd0, over
 * the network block device protocol, and a second one of another export as /dev/nbd1. Only with boot-arg
 * nbd=1 (and nbd1=1 for the second). See bsd/dev/vayu_nbd.c.
 */
#ifndef _SYS_VAYU_NBD_H_
#define _SYS_VAYU_NBD_H_

#include <stdbool.h>
#include <sys/types.h>

void vayu_nbd_init(void);
bool vayu_nbd_dev(dev_t dev);           /* /dev/nbd0, nbd1 (a request costs a round trip: worth putting together) */

#endif /* _SYS_VAYU_NBD_H_ */
