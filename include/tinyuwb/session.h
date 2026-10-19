/** @file session.h — credential/CCC ranging seam: map a credential session's URSK + M1-M4
 * setup to tu_ran_params. */

#ifndef TU_SESSION_H
#define TU_SESSION_H

#include <stdint.h>

#include "tinyuwb/mac.h"

/** SP0 MHR Vendor OUI for credential ranging (CSA Company Id). */
#define TU_CRED_VENDOR_OUI 0x4A191Bu

/**
 * @brief A credential ranging session: the URSK + the M1-M4 setup parameters; the interface to the
 * CCC MAC.
 */
struct tu_session {
	uint8_t ursk[TU_URSK_LEN]; /**< Ranging secret key (tinycred). */
	uint32_t uwb_session_id;    /**< M1: UWB Session Identifier. */
	uint32_t sts_index0;        /**< M4: STS_Index0. */
	uint32_t uwb_time0;         /**< M4: UWB_Time0 block-time anchor. */
	uint32_t hop_key_rw;        /**< M4: Hop Mode Key (HOP_Key_RW). */
	uint16_t mac_mode_offset;   /**< M3: MAC Mode round offset O^k (F = H + O). */
	uint8_t n_ran_s;            /**< M3: selected RAN multiplier N_RAN_S. */
	uint8_t n_chap_per_slot;    /**< M3: N_Chap_per_Slot. */
	uint8_t n_responder;        /**< M3: N_Responder. */
	uint8_t n_slot_per_round;   /**< M3: N_Slot_per_Round. */
	enum tu_hop_mode hop_mode; /**< M2/M3: hopping configuration. */
};

/** Rounds per ranging block, from N_RAN_S and the slot parameters (0 if the denominator is 0). */
uint16_t tu_session_n_round(const struct tu_session *s);

/** Map a credential session onto the CCC MAC's ranging-schedule parameters. */
int tu_session_to_ran_params(const struct tu_session *s, struct tu_ran_params *out);

#endif /* TU_SESSION_H */
