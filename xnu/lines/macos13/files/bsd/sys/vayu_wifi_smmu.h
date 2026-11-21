/* WCN399x's stage-1 SMMU context, before WLFW starts its firmware. */
#ifndef _SYS_VAYU_WIFI_SMMU_H_
#define _SYS_VAYU_WIFI_SMMU_H_

#include <stdbool.h>
#include <stdint.h>

/* Opt-in vayu_wifi_smmu=1. No full-DDR map and no overwrite of an existing
 * stream which already covers 0x640/0x641. */
bool vayu_wifi_smmu_enable(void);
/* Read-only FSR/FAR/syndrome report after a WLFW failure. */
void vayu_wifi_smmu_fault(const char *where);

/* W15-A: true only when this driver itself installed a usable stage-1 context. */
bool vayu_wifi_smmu_enabled(void);
/* Identity-map [pa, pa+size) in that context, 4 KiB leaves, WLAN_PAGE_NC.
 * Refuses to overwrite an existing leaf, re-verifies the MSA leaves, and
 * invalidates the context TLB.  false = no leaf was left installed. */
bool vayu_wifi_smmu_map_dma(uint64_t pa, uint64_t size, const char *what);
bool vayu_wifi_smmu_check_range(uint64_t pa, uint64_t size, const char *what);

#endif /* _SYS_VAYU_WIFI_SMMU_H_ */
