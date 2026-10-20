/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file sts.c — CCC STS substitution core (implementation). */

#include "tinyuwb/sts.h"

#include <errno.h>
#include <string.h>

/** @brief The bound session's key material, schedule anchor, and dURSK cache. */
static struct {
	bool active;
	uint32_t sts_index0;
	uint16_t n_slot_per_round;
	uint8_t mursk[TU_MURSK_LEN];
	uint8_t salted_hash[TU_SALTED_HASH_LEN];
	/* One-deep dURSK cache: dURSK is per ranging cycle, reused across a round of PPDUs. */
	bool cache_valid;
	uint32_t cache_base;
	uint8_t cache_dursk[TU_DURSK_LEN];
	/* One-deep dUDSK cache, same shape: dUDSK is per ranging cycle too, consumed once per
	 * Final_Data. */
	bool cache_dudsk_valid;
	uint32_t cache_dudsk_base;
	uint8_t cache_dudsk[TU_DUDSK_LEN];
	/* Blob-index calibration: origin + per-block stride learned from the first two indices
	 * (calib: 0 none, 1 origin, 2 stride). */
	uint8_t calib;
	uint32_t blob_origin;
	uint32_t blob_stride;
	/* Debug pin: emit the STS for one fixed CCC index so a sniffer keyed once validates every
	 * POLL. */
	bool pinned;
	uint32_t pin_index;
	/* Suspend: keep the derivation state live but make the per-frame IV wrap pass through. */
	bool suspend;
} rs;

/**
 * @brief Map a blob sub-block offset to a CCC slot (currently pass-through).
 * @param sub Sub-block offset.
 * @return CCC slot index.
 */
static uint32_t tu_sts_slot_from_sub(uint32_t sub)
{
	return sub;
}

/**
 * @brief Bind the shim to a ranging session's derived key material.
 * @param mursk mURSK bytes.
 * @param salted_hash SaltedHash bytes.
 * @param sts_index0 Initial STS index for this session.
 * @param n_slot_per_round Slots per ranging cycle.
 * @return 0 on success; -EINVAL if any input is invalid.
 */
int tu_sts_bind(const uint8_t mursk[TU_MURSK_LEN],
		  const uint8_t salted_hash[TU_SALTED_HASH_LEN], uint32_t sts_index0,
		  uint16_t n_slot_per_round)
{
	if (mursk == NULL || salted_hash == NULL || n_slot_per_round == 0u) {
		return -EINVAL;
	}
	memcpy(rs.mursk, mursk, sizeof(rs.mursk));
	memcpy(rs.salted_hash, salted_hash, sizeof(rs.salted_hash));
	rs.sts_index0 = sts_index0;
	rs.n_slot_per_round = n_slot_per_round;
	rs.cache_valid = false;
	rs.cache_dudsk_valid = false;
	rs.calib = 0u; /* re-learn the blob index origin/stride for this session */
	rs.active = true;
	return 0;
}

/**
 * @brief Bind the shim, deriving mURSK and SaltedHash from URSK and RangingConfiguration.
 * @param ursk 256-bit URSK input key.
 * @param ranging_config Serialized ranging configuration bytes (may be NULL if rc_len is 0).
 * @param rc_len Length of ranging_config in bytes.
 * @param sts_index0 Initial STS index for this session.
 * @param n_slot_per_round Slots per ranging cycle.
 * @return 0 on success; propagated error from derivation otherwise.
 */
int tu_sts_bind_from_ursk(const uint8_t ursk[TU_URSK_LEN], const uint8_t *ranging_config,
			    size_t rc_len, uint32_t sts_index0, uint16_t n_slot_per_round)
{
	uint8_t mursk[TU_MURSK_LEN];
	uint8_t salted_hash[TU_SALTED_HASH_LEN];
	int rc;

	if (ursk == NULL) {
		return -EINVAL;
	}
	rc = tu_derive_mursk(ursk, mursk);
	if (rc != 0) {
		return rc;
	}
	rc = tu_derive_salted_hash(ursk, ranging_config, rc_len, salted_hash);
	if (rc != 0) {
		return rc;
	}
	return tu_sts_bind(mursk, salted_hash, sts_index0, n_slot_per_round);
}

void tu_sts_unbind(void)
{
	memset(&rs, 0, sizeof(rs));
}

bool tu_sts_active(void)
{
	/* The per-frame IV wrap gates on this: bound AND not suspended. */
	return rs.active && !rs.suspend;
}

uint32_t tu_sts_index0(void)
{
	return rs.sts_index0;
}

/**
 * @brief Suspend or resume the per-frame IV wrap without unbinding.
 * @param suspend True to suspend, false to resume.
 */
void tu_sts_suspend(bool suspend)
{
	rs.suspend = suspend;
}

/**
 * @brief Map a per-frame STS index to its CCC dURSK (per-cycle) and STS-V (per-PPDU).
 * @param sts_index STS index in the ranging schedule.
 * @param dursk Output buffer receiving dURSK.
 * @param sts_v Output buffer receiving STS-V.
 * @return 0 on success; -EINVAL if shim is not active or output pointers are NULL.
 */
int tu_sts_for_index(uint32_t sts_index, uint8_t dursk[TU_DURSK_LEN],
			   uint8_t sts_v[TU_STS_V_LEN])
{
	uint32_t base;
	int rc;

	if (!rs.active || dursk == NULL || sts_v == NULL) {
		return -EINVAL;
	}

	/* Strip the slot offset to the cycle base: (sts_index - STS_Index0) mod N_Slot_per_Round is
	 * the slot offset. */
	base = sts_index - ((sts_index - rs.sts_index0) % rs.n_slot_per_round);

	/* dURSK is per ranging cycle; recompute only when the cycle changes. */
	if (!rs.cache_valid || rs.cache_base != base) {
		uint8_t ursk_kt[TU_URSK_KT_LEN];

		rc = tu_derive_ursk_kt(rs.mursk, base, ursk_kt);
		if (rc != 0) {
			return rc;
		}
		rc = tu_derive_dursk(ursk_kt, rs.salted_hash, rs.cache_dursk);
		if (rc != 0) {
			return rc;
		}
		rs.cache_base = base;
		rs.cache_valid = true;
	}
	memcpy(dursk, rs.cache_dursk, TU_DURSK_LEN);

	return tu_derive_sts_v(rs.salted_hash, sts_index, sts_v);
}

/**
 * @brief Derive dUDSK (per-cycle Final_Data key) for the ranging cycle containing sts_index.
 * @param sts_index STS index in the ranging schedule.
 * @param dudsk Output buffer receiving dUDSK.
 * @return 0 on success; -EINVAL if shim is not active or dudsk is NULL.
 */
int tu_sts_dudsk_for_index(uint32_t sts_index, uint8_t dudsk[TU_DUDSK_LEN])
{
	uint32_t base;
	uint8_t ursk_kt[TU_URSK_KT_LEN];
	int rc;

	if (!rs.active || dudsk == NULL) {
		return -EINVAL;
	}
	/* Same per-cycle base as the STS path; dUDSK shares URSK_KT, differs only by the KDF label.
	 * One-deep cache on the base, like the dURSK path above: the value is identical for every
	 * Final_Data in a cycle. */
	base = sts_index - ((sts_index - rs.sts_index0) % rs.n_slot_per_round);
	if (!rs.cache_dudsk_valid || rs.cache_dudsk_base != base) {
		rc = tu_derive_ursk_kt(rs.mursk, base, ursk_kt);
		if (rc != 0) {
			return rc;
		}
		rc = tu_derive_dudsk(ursk_kt, rs.salted_hash, rs.cache_dudsk);
		if (rc != 0) {
			return rc;
		}
		rs.cache_dudsk_base = base;
		rs.cache_dudsk_valid = true;
	}
	memcpy(dudsk, rs.cache_dudsk, TU_DUDSK_LEN);

	return 0;
}

/**
 * @brief Map a ranging-slot offset (STS_Index0 plus slot) to its CCC dURSK and STS-V.
 * @param slot Slot offset from STS_Index0.
 * @param dursk Output buffer receiving dURSK.
 * @param sts_v Output buffer receiving STS-V.
 * @return 0 on success; -EINVAL if shim is not active.
 */
int tu_sts_for_slot(uint32_t slot, uint8_t dursk[TU_DURSK_LEN], uint8_t sts_v[TU_STS_V_LEN])
{
	if (!rs.active) {
		return -EINVAL;
	}
	/* Absolute index = STS_Index0 + slot; tu_sts_for_index strips it to the per-cycle
	 * base. */
	return tu_sts_for_index(rs.sts_index0 + slot, dursk, sts_v);
}

/**
 * @brief Extract the STS index from a DW3000 STS IV (index at bytes 7..4).
 * @param iv16 16-byte STS IV.
 * @return STS index.
 */
uint32_t tu_sts_index_from_iv(const uint8_t iv16[16])
{
	/* Bench-confirmed layout (2026-07-06) — see tu_shim.h. */
	return ((uint32_t)iv16[7] << 24) | ((uint32_t)iv16[6] << 16) | ((uint32_t)iv16[5] << 8) |
	       (uint32_t)iv16[4];
}

/**
 * @brief Map the blob's raw provisioned STS index to a CCC-schedule STS index, with origin and
 * stride auto-calibrated from the first two indices.
 * @param blob_idx Blob provisioned STS index.
 * @param block Output pointer receiving the ranging-cycle block index (may be NULL).
 * @param sub Output pointer receiving the intra-block sub-offset (may be NULL).
 * @return CCC-space STS index.
 */
uint32_t tu_sts_blob_to_index(uint32_t blob_idx, uint32_t *block, uint32_t *sub)
{
	uint32_t rel, blk, off;

	/* Auto-calibrate the blob's index origin (1st frame) then per-block stride (2nd); a zero
	 * delta holds at calib=1. */
	if (rs.calib == 0u) {
		rs.blob_origin = blob_idx;
		rs.calib = 1u;
	} else if (rs.calib == 1u && blob_idx != rs.blob_origin) {
		rs.blob_stride = blob_idx - rs.blob_origin;
		rs.calib = 2u;
	}

	rel = blob_idx - rs.blob_origin; /* uint32 wraps with the STS-index space */
	if (rs.blob_stride != 0u) {
		blk = rel / rs.blob_stride;
		off = rel % rs.blob_stride;
	} else {
		/* Stride not learned yet (frames 0..1): treat as block 0. */
		blk = 0u;
		off = rel;
	}

	if (block != NULL) {
		*block = blk;
	}
	if (sub != NULL) {
		*sub = off;
	}

	/* Debug pin: emit a constant STS regardless of the blob index; block/sub still reflect the
	 * real advance for the log. */
	if (rs.pinned) {
		return rs.pin_index;
	}

	/* Re-express in CCC-index space; per-block CCC stride defaults to N_Slot_per_Round, slot
	 * hook places the frame in the round. */
	return rs.sts_index0 + blk * (uint32_t)rs.n_slot_per_round + tu_sts_slot_from_sub(off);
}

/**
 * @brief Pin the substituted STS to one fixed CCC index (bench validation).
 * @param tu_index CCC index to pin for all subsequent frames.
 */
void tu_sts_pin_index(uint32_t tu_index)
{
	rs.pin_index = tu_index;
	rs.pinned = true;
}

void tu_sts_unpin(void)
{
	rs.pinned = false;
}
