/* vayu IPA 4.1 / GSI 2.0 packet transport (separate from the modem QMI handshake). */
#ifndef _SYS_VAYU_IPA_DATA_H_
#define _SYS_VAYU_IPA_DATA_H_

#ifdef KERNEL
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* After ipa_fws and IPA HW init, before MPSS: channels, endpoints and empty tables.
 * vayu_ipa_data=1 opts in; 0 leaves only the control-plane handshake available. */
bool vayu_ipa_data_prepare(void);
/* False if the polling thread or a channel did not stop: caller must leave IPA powered. */
bool vayu_ipa_data_stop(void);
/* True only when all four channels started, RX was primed and all table commands completed.
 * A modem filter request which is not implemented clears this again; WDS must fail closed. */
bool vayu_ipa_data_ready(void);
void vayu_ipa_data_filter_error(void);
/* Raw IP packet, copied into a QMAP/TX slot before returning. */
bool vayu_ipa_data_transmit(uint8_t mux, const void *packet, size_t length);
#endif

#endif /* _SYS_VAYU_IPA_DATA_H_ */
