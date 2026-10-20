/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file mac.h — CCC UWB MAC layer: ranging-round scheduling, SP0 frame codec, DS-TWR. */

#ifndef TU_MAC_H
#define TU_MAC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyuwb/kdf.h"
#include "tinyuwb/twr.h"

/** Default hopping round index S(i) in [0, n_round) for a ranging block, keyed by HOP_Key_RW. */
uint16_t tu_hop_round_index(uint32_t block_index, uint32_t hop_key_rw, uint32_t n_round);

/* ── SP0 frame codec ──────────────────────────────────────────────────────── */

/** SP0 MHR length, bytes. */
#define TU_MHR_LEN            23
/** Pre-POLL MAC payload length, bytes. */
#define TU_PRE_POLL_LEN       13
/** Final_Data payload length before the responder records, bytes. */
#define TU_FINAL_DATA_HDR_LEN 18
/** One responder timestamp record in a Final_Data payload, bytes. */
#define TU_RESPONDER_LEN      7
/** Max responders in one Final_Data (127-byte payload cap). */
#define TU_MAX_RESPONDERS     10
/** Vendor Specific Information message ID for Pre-POLL. */
#define TU_MSG_ID_PRE_POLL    0x01u
/** Vendor Specific Information message ID for Final_Data. */
#define TU_MSG_ID_FINAL_DATA  0x02u

/**
 * @brief The per-frame-variable fields of an SP0 MHR (fixed fields are built in by tu_build_mhr).
 */
struct tu_mhr_fields {
	uint16_t dest_short_addr;              /**< Destination Short Address. */
	uint32_t frame_counter;                /**< Aux Security Header frame counter. */
	uint8_t key_source[TU_KEYSOURCE_LEN]; /**< Aux Security Header Key Source. */
	uint8_t msg_id;                        /**< Vendor Info byte 1 (@c TU_MSG_ID_*). */
	uint8_t payload_len;                   /**< Vendor Info byte 2 (MAC payload length). */
};

/** Build the 23-byte SP0 MHR (little-endian on the wire). */
int tu_build_mhr(const struct tu_mhr_fields *f, uint8_t out[TU_MHR_LEN]);

/** Parse and validate a 23-byte SP0 MHR, extracting the variable fields (-EINVAL on mismatch). */
int tu_parse_mhr(const uint8_t in[TU_MHR_LEN], struct tu_mhr_fields *f);

/**
 * @brief Pre-POLL request message parameters.
 */
struct tu_pre_poll {
	uint32_t uwb_session_id; /**< ID of the UWB ranging session. */
	uint32_t poll_sts_index; /**< STS index of the succeeding POLL message. */
	uint16_t ranging_block;  /**< Session's index of the current ranging block. */
	uint8_t hop_flag;        /**< Hop flag for the current ranging block. */
	uint16_t round_index;    /**< Ranging round index for the current block. */
};

/** Pack a Pre-POLL payload little-endian. */
int tu_pre_poll_pack(const struct tu_pre_poll *p, uint8_t out[TU_PRE_POLL_LEN]);

/** Parse a 13-byte Pre-POLL payload. */
int tu_pre_poll_parse(const uint8_t in[TU_PRE_POLL_LEN], struct tu_pre_poll *p);

/**
 * @brief One responder's timestamp record in a Final_Data message.
 */
struct tu_responder_ts {
	uint8_t responder_index;       /**< Index of the responder this record refers to. */
	uint32_t timestamp;            /**< POLL→RESPONSE time difference at the initiator. */
	uint8_t timestamp_uncertainty; /**< Ranging timestamp uncertainty. */
	uint8_t ranging_status;        /**< Per-responder ranging status. */
};

/**
 * @brief Final_Data message parameters.
 */
struct tu_final_data {
	uint32_t uwb_session_id;      /**< ID of the UWB ranging session. */
	uint16_t ranging_block;       /**< Index of the current ranging block. */
	uint8_t hop_flag;             /**< Hop flag for the next ranging block. */
	uint16_t round_index;         /**< Ranging round for the next ranging block. */
	uint32_t final_sts_index;     /**< STS index of the preceding FINAL message. */
	uint32_t ranging_ts_final_tx; /**< POLL→Final transmit time difference. */
	uint8_t num_responders;       /**< Responder records that follow (≤ TU_MAX_RESPONDERS). */
	struct tu_responder_ts responders[TU_MAX_RESPONDERS];
};

/** Pack a Final_Data payload little-endian. */
int tu_final_data_pack(const struct tu_final_data *f, uint8_t *out, size_t cap, size_t *len);

/** Parse a Final_Data payload (-EINVAL if length is inconsistent with num_responders). */
int tu_final_data_parse(const uint8_t *in, size_t len, struct tu_final_data *f);

/* ── Ranging schedule ─────────────────────────────────────────────────────── */

/** Hopping mode.  Adaptive (interference-driven) is out of scope. */
enum tu_hop_mode {
	TU_HOP_NONE = 0,       /**< No hopping: every block uses ranging round 0. */
	TU_HOP_CONTINUOUS = 1, /**< Continuous hopping: block i uses round S(i). */
};

/** Slot role within a ranging round. */
enum tu_slot {
	TU_SLOT_PRE_POLL,   /**< Slot 0 — SP0 data packet, initiator. */
	TU_SLOT_POLL,       /**< Slot 1 — SP3 RFRAME, initiator. */
	TU_SLOT_RESPONSE,   /**< Slots 2..1+N — SP3 RFRAME, responder l. */
	TU_SLOT_FINAL,      /**< SP3 RFRAME, initiator. */
	TU_SLOT_FINAL_DATA, /**< SP0 data packet, initiator. */
};

/**
 * @brief Per-session ranging schedule parameters (negotiated in setup).
 */
struct tu_ran_params {
	uint32_t sts_index0;        /**< STS_Index0 — ranging round 0 of block 0. */
	uint16_t n_slot_per_round;  /**< N_Slot_per_Round. */
	uint16_t n_round;           /**< N_Round — rounds per block (> 0). */
	uint8_t n_responder;        /**< N_Responder (1..TU_MAX_RESPONDERS). */
	uint32_t hop_key_rw;        /**< HOP_Key_RW (continuous hopping). */
	enum tu_hop_mode hop_mode; /**< Hopping mode. */
};

/**
 * @brief The initiator's next-block hop decision, carried in Final_Data.
 */
struct tu_hop_decision {
	uint8_t hop_flag;     /**< Hop_Flag for the next ranging block. */
	uint16_t round_index; /**< Round_Index for the next ranging block. */
};

/** The ranging round a block uses (block 0 uses round 0; continuous hopping uses S(i) for i>=1). */
uint16_t tu_block_round(const struct tu_ran_params *p, uint32_t block);

/** STS index for one slot of a ranging round (uint32 wraps mod 2^32). */
uint32_t tu_slot_sts_index(const struct tu_ran_params *p, uint32_t block, uint16_t round,
			    enum tu_slot slot, uint8_t responder);

/** The initiator's hop decision for the block after block, written into its Final_Data. */
struct tu_hop_decision tu_initiator_next_hop(const struct tu_ran_params *p, uint32_t block);

/* ── Double-sided two-way ranging ─────────────────────────────────────────── */

/* The intervals and the estimator itself live at the base UWB tier (ds_twr.h),
 * because the anchor-to-anchor link needs them without a CCC engine. What stays
 * here is the CCC-specific half: filling those intervals from a Final_Data. */

/** Assemble the DS-TWR intervals at the responder from a received Final_Data. */
int tu_responder_twr(const struct tu_final_data *fd, uint8_t responder, uint32_t t_reply1,
			 uint32_t t_round2, struct tu_twr *out);

/* ── URSK lifetime ────────────────────────────────────────────────────────── */

/** STS index at which the URSK must be discarded: 2^31 − 1. */
#define TU_STS_INDEX_MAX 0x7FFFFFFFu

/** Whether the current URSK is exhausted for a ranging block (true once its highest STS index would
 * exceed 2^31-1). */
bool tu_ursk_exhausted(const struct tu_ran_params *p, uint32_t block);

#endif /* TU_MAC_H */
