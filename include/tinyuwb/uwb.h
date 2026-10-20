/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file uwb.h — tinyuwb's public API: the credential DS-TWR responder's lifecycle and the
 * range it measures. Everything above this header speaks these functions and never names
 * the radio. */

#ifndef TINYUWB_UWB_H
#define TINYUWB_UWB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Ranging parameters negotiated during the credential M1-M4 handshake. */
struct tu_cfg {
	uint32_t session_id;         /**< UWB session id (any non-zero). */
	uint8_t channel;             /**< UWB channel: 5 or 9. */
	uint8_t sync_code_index;     /**< SYNC/preamble code index (1..32). */
	uint16_t slot_duration_rstu; /**< Slot duration, RSTU (1200 = 1 ms). */
	uint32_t block_duration_ms;  /**< Ranging block period, ms. */
	uint8_t slot_per_round;      /**< Slots per ranging round. */
	uint32_t sts_index0;         /**< Starting STS index. */
	uint64_t uwb_time_us;        /**< UWB_Time0 initiation reference, us. */
	const uint8_t *ursk;         /**< 32-byte URSK (provisioned STS root). */
	const uint8_t *ranging_config; /**< Serialized RangingConfiguration (SaltedHash input), or NULL. */
	size_t rc_len;               /**< RangingConfiguration length, bytes (17). */
};

/** Start the DS-TWR responder bound to a live credential; 0 on success. */
int tu_start(const struct tu_cfg *cfg);

/** Pre-apply the expected session PHY so the M4-time start skips the configure. */
int tu_prewarm(uint8_t channel, uint8_t sync_code_index);

/** Quiesce the radio and unbind the STS keying. */
void tu_stop(void);

/** Latest distance in cm; true if a valid range has been seen. */
bool tu_last_range_cm(int32_t *cm_out);
bool tu_last_range_age_cm(int32_t *cm_out, int64_t *age_ms_out);

/** Latest distance, gated by the range-integrity consensus: the accessor an unlock
 * decision must use. */
bool tu_trusted_range_cm(int32_t *cm_out);
bool tu_trusted_range_age_cm(int32_t *cm_out, int64_t *age_ms_out);
bool tu_trusted_range_block_cm(int32_t *cm_out, uint32_t *block_out);

/** The live UWB session id, or 0 when no session is up. */
uint32_t tu_session_id(void);

/** Monotonic accepted-range epoch for post-challenge freshness checkpoints. */
uint32_t tu_range_generation(void);

/** Counts successful tu_start() calls. */
uint32_t tu_start_generation(void);

/** Trusted distance only when its accepted-range epoch is newer than @p after. */
bool tu_trusted_range_after_cm(int32_t *cm_out, uint32_t after);

/** Layer-2 evidence for a latched range, for a consumer that must fail closed. */
struct tu_uwb_range_integrity {
	bool sts_ok;
	int16_t sts_quality;
	uint8_t trust_level;
};

bool tu_trusted_range_after_checked_cm(int32_t *cm_out, uint32_t after,
				       struct tu_uwb_range_integrity *ig_out);

/** Callback fired after each accepted range latch (NULL to clear); runs on the radio's
 * event path, keep it to a wake. */
void tu_set_range_listener(void (*cb)(void));

/** Per-session step tallies of the ranging round. */
struct tu_round_stats {
	uint32_t prepoll_ok;   /**< Pre-POLLs accepted (MIC + context + replay checks) */
	uint32_t warm_fail;    /**< next-block STS derivations that failed */
	uint32_t poll_arm;     /**< SP3 POLL windows armed */
	uint32_t arm_fail;     /**< POLL windows the radio refused to arm */
	uint32_t poll_ok;      /**< POLLs received with a good STS */
	uint32_t poll_fail;    /**< POLL windows that closed without one */
	uint32_t resp_tx;      /**< Response delayed TXs armed */
	uint32_t final_data;   /**< Final_Data frames decoded */
	uint32_t range;        /**< ranges latched */
	uint32_t last_poll_st; /**< flags of the last POLL window result */
};
void tu_round_stats_get(struct tu_round_stats *out);

/** True while the Pre-POLL listener is up. */
bool tu_listening(void);

/* ── Round entry points also reachable from tests ───────────────────────── */

/** The RX result handler alone (arm, respond, revert), without the Pre-POLL decode. */
void tu_round_rx_event(uint32_t flags, uint16_t len);

/** Stash the received SP0 frame and decode it (Pre-POLL deferred, Final_Data inline). */
void tu_round_try_prepoll(uint16_t len);

/** True while an SP3 POLL RX is armed. */
bool tu_round_awaiting_poll(void);

#endif /* TINYUWB_UWB_H */
