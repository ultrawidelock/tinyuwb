/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file test_schedule.c — the ranging schedule an initiator publishes.
 *
 * The initiator's Pre-POLL announces a Poll_STS_Index, and the responder does not compute
 * one: it reads what it is told and learns the per-block stride by subtracting consecutive
 * announcements. The schedule's properties are pinned here with the M3 parameters a bench
 * reader sends. */
#include <errno.h>
#include <stdbool.h>

#include "test.h"
#include "tinyuwb/mac.h"
#include "tinyuwb/session.h"

#define M3_RAN_MULT        4u
#define M3_CHAPS_PER_SLOT  3u
#define M3_RESPONDERS      1u
#define M3_SLOTS_PER_ROUND 12u
#define M4_STS_INDEX0      0x1000u
#define M4_HOP_KEY         0x11223344u

/* 288 * 4 / (3 * 12) = 32. */
#define EXPECT_N_ROUND 32u
/* One block advances the index by N_Slot_per_Round * N_Round = 12 * 32. */
#define EXPECT_STRIDE 384u

static struct tu_session bench(enum tu_hop_mode hop)
{
	struct tu_session s = {
		.uwb_session_id = 0x0badf00du,
		.sts_index0 = M4_STS_INDEX0,
		.uwb_time0 = 0u,
		.hop_key_rw = M4_HOP_KEY,
		.mac_mode_offset = 0u,
		.n_ran_s = M3_RAN_MULT,
		.n_chap_per_slot = M3_CHAPS_PER_SLOT,
		.n_responder = M3_RESPONDERS,
		.n_slot_per_round = M3_SLOTS_PER_ROUND,
		.hop_mode = hop,
	};
	return s;
}

void test_schedule(void)
{
	struct tu_session s = bench(TU_HOP_NONE);
	struct tu_ran_params p;
	uint32_t prev, stride;

	t_group("bench M3 forms a schedule");
	T_EQ("map.ok", tu_session_to_ran_params(&s, &p), 0);
	T_EQ("map.n_round", p.n_round, EXPECT_N_ROUND);
	T_EQ("map.n_slot", p.n_slot_per_round, M3_SLOTS_PER_ROUND);
	T_EQ("map.sts0", p.sts_index0, M4_STS_INDEX0);

	t_group("the round holds every packet of a block");
	T_EQ("slot.prepoll", tu_slot_sts_index(&p, 0u, 0u, TU_SLOT_PRE_POLL, 0u), M4_STS_INDEX0);
	T_EQ("slot.poll", tu_slot_sts_index(&p, 0u, 0u, TU_SLOT_POLL, 0u), M4_STS_INDEX0 + 1u);
	T_EQ("slot.resp0", tu_slot_sts_index(&p, 0u, 0u, TU_SLOT_RESPONSE, 0u),
	     M4_STS_INDEX0 + 2u);
	T_EQ("slot.final", tu_slot_sts_index(&p, 0u, 0u, TU_SLOT_FINAL, 0u),
	     M4_STS_INDEX0 + M3_RESPONDERS + 2u);
	T_EQ("slot.final_data", tu_slot_sts_index(&p, 0u, 0u, TU_SLOT_FINAL_DATA, 0u),
	     M4_STS_INDEX0 + M3_RESPONDERS + 3u);
	T_OK("slots.fit_round", (uint32_t)M3_RESPONDERS + 3u < M3_SLOTS_PER_ROUND);

	t_group("no hopping: every block uses round 0");
	{
		bool all_zero = true;

		for (uint32_t b = 0u; b < 64u; b++) {
			if (tu_block_round(&p, b) != 0u) {
				all_zero = false;
				break;
			}
		}
		T_OK("hop_none.round0", all_zero);
	}

	t_group("the stride the responder learns is constant");
	prev = tu_slot_sts_index(&p, 0u, tu_block_round(&p, 0u), TU_SLOT_POLL, 0u);
	T_EQ("stride.block0", prev, M4_STS_INDEX0 + 1u);
	stride = EXPECT_STRIDE;
	for (uint32_t b = 1u; b < 64u; b++) {
		uint32_t idx = tu_slot_sts_index(&p, b, tu_block_round(&p, b), TU_SLOT_POLL, 0u);

		stride = idx - prev;
		if (stride != EXPECT_STRIDE) {
			break;
		}
		prev = idx;
	}
	T_EQ("stride.constant", stride, EXPECT_STRIDE);

	t_group("continuous hopping would not give a constant stride");
	{
		struct tu_session h = bench(TU_HOP_CONTINUOUS);
		struct tu_ran_params hp;
		bool varies = false;

		T_EQ("hop.map.ok", tu_session_to_ran_params(&h, &hp), 0);
		prev = tu_slot_sts_index(&hp, 0u, tu_block_round(&hp, 0u), TU_SLOT_POLL, 0u);
		for (uint32_t b = 1u; b < 64u; b++) {
			uint32_t idx =
				tu_slot_sts_index(&hp, b, tu_block_round(&hp, b), TU_SLOT_POLL, 0u);

			if (idx - prev != EXPECT_STRIDE) {
				varies = true;
				break;
			}
			prev = idx;
		}
		T_OK("hop.stride_varies", varies);
	}

	t_group("the URSK outlives a bench run");
	T_OK("ursk.block0", !tu_ursk_exhausted(&p, 0u));
	T_OK("ursk.block_1e6", !tu_ursk_exhausted(&p, 1000000u));

	t_group("bad M3 stops the transmitter");
	{
		struct tu_session tight = bench(TU_HOP_NONE);

		tight.n_slot_per_round = M3_RESPONDERS + 3u; /* one slot short */
		T_EQ("bad.slots_short", tu_session_to_ran_params(&tight, &p), -EINVAL);
	}
	{
		struct tu_session slow = bench(TU_HOP_NONE);

		slow.n_slot_per_round = 200u;
		slow.n_chap_per_slot = 100u;
		T_EQ("bad.zero_round", tu_session_to_ran_params(&slow, &p), -EINVAL);
	}
}
