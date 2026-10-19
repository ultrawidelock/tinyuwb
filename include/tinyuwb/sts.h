/** @file sts.h — map a per-frame STS index to the (dURSK, STS-V) pair the STS engine loads. */

#ifndef TINYUWB_STS_H
#define TINYUWB_STS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyuwb/kdf.h"

/** Bind the keying to a ranging session's derived key material. */
int tu_sts_bind(const uint8_t mursk[TU_MURSK_LEN], const uint8_t salted_hash[TU_SALTED_HASH_LEN],
		uint32_t sts_index0, uint16_t n_slot_per_round);

/** Bind the keying, deriving mURSK + SaltedHash from the URSK + RangingConfiguration. */
int tu_sts_bind_from_ursk(const uint8_t ursk[TU_URSK_LEN], const uint8_t *ranging_config,
			  size_t rc_len, uint32_t sts_index0, uint16_t n_slot_per_round);

/** @brief Unbind; @ref tu_sts_active returns false afterward. */
void tu_sts_unbind(void);

/** @brief Whether the per-frame STS substitution is live (bound AND not suspended). */
bool tu_sts_active(void);

/** Suspend/resume the per-frame substitution without unbinding. */
void tu_sts_suspend(bool suspend);

/** Map a per-frame STS index to its dURSK (per-cycle) + STS-V (per-PPDU). */
int tu_sts_for_index(uint32_t sts_index, uint8_t dursk[TU_DURSK_LEN], uint8_t sts_v[TU_STS_V_LEN]);

/** Derive the dUDSK (per-cycle Final_Data key) for the ranging cycle containing sts_index. */
int tu_sts_dudsk_for_index(uint32_t sts_index, uint8_t dudsk[TU_DUDSK_LEN]);

/** Map a ranging-slot offset (STS_Index0 + slot) to its dURSK + STS-V. */
int tu_sts_for_slot(uint32_t slot, uint8_t dursk[TU_DURSK_LEN], uint8_t sts_v[TU_STS_V_LEN]);

/** Extract the STS index packed into a 16-byte STS IV (index at bytes 7..4). */
uint32_t tu_sts_index_from_iv(const uint8_t iv16[16]);

/** Map a raw provisioned STS index to a schedule STS index (origin/stride auto-calibrated from
 * the first two indices). */
uint32_t tu_sts_blob_to_index(uint32_t blob_idx, uint32_t *block, uint32_t *sub);

/** Pin the substituted STS to one fixed index (bench validation). */
void tu_sts_pin_index(uint32_t index);

/** @brief Release the pin; the STS resumes advancing with the index. */
void tu_sts_unpin(void);

/** @brief The bound session's `STS_Index0` (for UAD/Pre-POLL derivation); 0 if unbound. */
uint32_t tu_sts_index0(void);

#endif /* TINYUWB_STS_H */
