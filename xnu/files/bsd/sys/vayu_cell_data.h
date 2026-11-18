/* vayu cellular data-session control plane (DPM/WDA/WDS), not the packet data path. */
#ifndef _SYS_VAYU_CELL_DATA_H_
#define _SYS_VAYU_CELL_DATA_H_

#ifdef KERNEL
#include <stdbool.h>

struct vayu_qrtr;

/* Enabled by vayu_cell_data=1 (legacy full path) or vayu_cell_lte=1 (control plane after the IPA
 * QMI handshake; APN internet by default) after vayu_cell_start(). */
void vayu_cell_data_start(struct vayu_qrtr *q);
void vayu_cell_data_stop(void);
/* The NAS client reports the current registration; data setup starts only on true. */
void vayu_cell_data_registered(bool registered);
/* NAS should poll promptly while registration/data setup is pending. */
bool vayu_cell_data_pending(void);
#endif

#endif /* _SYS_VAYU_CELL_DATA_H_ */
