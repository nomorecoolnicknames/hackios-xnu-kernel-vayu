/* vayu IPA 4.1 bring-up and its modem-facing readiness gate. */
#ifndef _SYS_VAYU_IPA_H_
#define _SYS_VAYU_IPA_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stdint.h>

struct vayu_qrtr;

/* Pre-modem stage: IPA PAS, clock, memory/SMMU and SMP2P prerequisites (opt-in vayu_ipa=1). */
void vayu_ipa_prepare(void);
/* After MPSS is up (vayu_modem.c step 2): -1 vayu_ipa not asked, 0 IPA/GSI not up, 1 up.
 * With vayu_ipa_loader=1 the AP does no PAS, so 1 means the modem brought IPA/GSI up itself. */
int vayu_ipa_modem_probe(void);
/* QRTR stage: modem's IPA service 0x31, INIT_MODEM_DRIVER and the host indication path. */
void vayu_ipa_start(struct vayu_qrtr *q);
void vayu_ipa_stop(void);
void vayu_ipa_prepare_stop(void);
bool vayu_ipa_smmu_enable(void);
/* Only true after the modem accepted INIT_MODEM_DRIVER and INIT_COMPLETE was sent. WDS Start must
 * not run before this: 0641 showed ipa_ipfltr.init_done FATAL without it. */
bool vayu_ipa_ready(void);
/* True when a run was explicitly enabled but has not become ready; useful to explain the gate. */
bool vayu_ipa_pending(void);
/* AP->modem and modem->AP IPA endpoint IDs from the vayu IPA 4.1 table (2 and 11). */
void vayu_ipa_modem_endpoints(uint32_t *to_modem, uint32_t *from_modem);
/* Read IPA_SHARED_MEM_SIZE-derived restricted bytes, required by INIT_MODEM_DRIVER. */
uint32_t vayu_ipa_restricted_bytes(void);
#endif

#endif /* _SYS_VAYU_IPA_H_ */
