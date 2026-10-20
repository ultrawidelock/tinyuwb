/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file test_range.c — range + URSK store. */
#include <string.h>

#include "tinyuwb/kdf.h"
#include "tinyuwb/range.h"
#include "test.h"

static int s_listener_hits;

static void range_listener(void)
{
	s_listener_hits++;
}

void test_range(void)
{
	uint8_t ursk[TU_URSK_LEN];
	int32_t cm = -1;
	uint32_t block = 0;
	int64_t age = -1;

	for (size_t i = 0; i < sizeof(ursk); i++) {
		ursk[i] = (uint8_t)(i + 1u);
	}

	t_group("range store starts empty");
	tu_rs_set_provisioned_ursk(NULL); /* also normalises state */
	T_OK("empty", !tu_rs_last_range(&cm, NULL, NULL, NULL, NULL));

	t_group("URSK stash");
	T_OK("ursk.none", tu_rs_get_ursk() == NULL);
	tu_rs_set_provisioned_ursk(ursk);
	const uint8_t *g = tu_rs_get_ursk();
	T_OK("ursk.set", g != NULL && memcmp(g, ursk, sizeof(ursk)) == 0);
	tu_rs_set_provisioned_ursk(NULL);
	T_OK("ursk.cleared", tu_rs_get_ursk() == NULL);

	t_group("diagnostic slot counter is inert");
	T_EQ("slot", tu_rs_current_slot(), 0u);

	t_group("latch + read range");
	tu_rs_latch_cm(150, 7u);
	T_OK("present", tu_rs_last_range(&cm, NULL, NULL, &block, &age));
	T_EQ("cm", cm, 150);
	T_EQ("block", block, 7u);
	T_OK("age.nonneg", age >= 0);
	/* Negative distance clamps to 0. */
	tu_rs_latch_cm(-5, 9u);
	tu_rs_last_range(&cm, NULL, NULL, &block, NULL);
	T_EQ("cm.clamped", cm, 0);
	T_EQ("block2", block, 9u);
	/* All out-params optional. */
	T_OK("null.outs", tu_rs_last_range(NULL, NULL, NULL, NULL, NULL));
	/* All out-params filled: the CCC latch reports addr 0, LOS. */
	uint16_t addr = 0xFFFFu;
	uint8_t nlos = 0xFFu;
	T_OK("full.outs", tu_rs_last_range(&cm, &addr, &nlos, &block, &age));
	T_EQ("addr", addr, 0u);
	T_EQ("nlos", nlos, 0u);

	/* ── Range-integrity gate ─────────────────────────────────────────────── */

	t_group("layer 1: plausibility predicate");
	T_OK("plaus.150", tu_rs_plausible(150));
	T_OK("plaus.zero", tu_rs_plausible(0));
	T_OK("plaus.small_neg", tu_rs_plausible(-TU_RANGE_NEG_TOL_CM));
	T_OK("plaus.max", tu_rs_plausible(TU_RANGE_MAX_CM));
	T_OK("implaus.big_neg", !tu_rs_plausible(-400));
	T_OK("implaus.below_tol", !tu_rs_plausible(-TU_RANGE_NEG_TOL_CM - 1));
	T_OK("implaus.over_max", !tu_rs_plausible(TU_RANGE_MAX_CM + 1));

	t_group("layer 1: Ghost-Peak negative is dropped, not clamped to 0");
	tu_rs_latch_cm(120, 20u); /* a good close read */
	tu_rs_last_range(&cm, NULL, NULL, &block, NULL);
	T_EQ("good.latched", cm, 120);
	/* The reported exploit: a hugely negative ToF must NOT latch as a valid 0 cm. */
	tu_rs_latch_cm(-400, 21u);
	tu_rs_last_range(&cm, NULL, NULL, &block, NULL);
	T_EQ("exploit.not_zero", cm, 120);   /* kept last good; did NOT become 0 */
	T_EQ("exploit.block_kept", block, 20u); /* the spoof block never latched */
	/* An over-envelope outlier is likewise dropped. */
	tu_rs_latch_cm(TU_RANGE_MAX_CM + 500, 22u);
	tu_rs_last_range(&cm, NULL, NULL, NULL, NULL);
	T_EQ("overmax.dropped", cm, 120);
	/* Small negative is legit point-blank slop and still reads as 0. */
	tu_rs_latch_cm(-3, 23u);
	tu_rs_last_range(&cm, NULL, NULL, NULL, NULL);
	T_EQ("small_neg.zero", cm, 0);

	t_group("layer 2: STS-quality floor");
	T_OK("sts.good", tu_rs_sts_quality_ok(0, 100));
	T_OK("sts.good_at_floor", tu_rs_sts_quality_ok(5, TU_STS_QUALITY_MIN));
	T_OK("sts.bad_verdict", !tu_rs_sts_quality_ok(-1, 100));
	T_OK("sts.below_floor",
	     !tu_rs_sts_quality_ok(0, (int16_t)(TU_STS_QUALITY_MIN - 1)));

	t_group("layer 4: cross-block consensus builds and breaks trust");
	tu_rs_latch_cm(90, 30u);
	tu_rs_latch_cm(92, 31u); /* within SPREAD of 90 */
	T_OK("trust.building", !tu_rs_trusted()); /* only 2 agreeing */
	tu_rs_latch_cm(91, 32u); /* 3rd agreeing -> K reached */
	T_OK("trust.reached", tu_rs_trusted());
	/* A lone plausible outlier breaks trust but still latches (live telemetry). */
	tu_rs_latch_cm(300, 33u); /* jump > SPREAD */
	tu_rs_last_range(&cm, NULL, NULL, NULL, NULL);
	T_EQ("outlier.latched", cm, 300);
	T_OK("outlier.untrusted", !tu_rs_trusted());
	/* Rebuild trust, then a spoof (implausible) block clears it and does not latch. */
	tu_rs_latch_cm(305, 34u);
	tu_rs_latch_cm(307, 35u);
	tu_rs_latch_cm(303, 36u);
	T_OK("trust.rebuilt", tu_rs_trusted());
	tu_rs_latch_cm(-500, 37u); /* Ghost-Peak */
	T_OK("spoof.clears_trust", !tu_rs_trusted());
	tu_rs_last_range(&cm, NULL, NULL, &block, NULL);
	T_EQ("spoof.range_kept", cm, 303);
	T_EQ("spoof.block_kept", block, 36u);

	t_group("layer 4: trust saturates at K and an outlier forces a full rebuild");
	tu_rs_latch_cm(200, 40u);
	tu_rs_latch_cm(202, 41u);
	tu_rs_latch_cm(201, 42u); /* K reached */
	tu_rs_latch_cm(203, 43u); /* extra agreeing: trust caps at K */
	tu_rs_latch_cm(199, 44u);
	T_OK("sat.trusted", tu_rs_trusted());
	/* One jump > SPREAD must drop trust to a single fresh sample, so a lone
	 * agreeing block after it is NOT enough — a full K-run has to rebuild. */
	tu_rs_latch_cm(400, 45u); /* jump -> run restarts at 1 */
	T_OK("sat.jump.untrusted", !tu_rs_trusted());
	tu_rs_latch_cm(402, 46u); /* 2 */
	T_OK("sat.one_after_jump_insufficient", !tu_rs_trusted());
	tu_rs_latch_cm(401, 47u); /* 3 -> K reached again */
	T_OK("sat.full_rebuild_trusted", tu_rs_trusted());

	t_group("layer 2: evidence recorded with the range");
	struct tu_rs_integrity ig;

	tu_rs_reset_ranges();
	T_OK("ig.none_before_a_range", !tu_rs_last_range_integrity(&ig));
	/* Latches with no preceding evidence call must read as a failed STS. This is
	 * the fail-closed default the whole design rests on: a producer that says
	 * nothing about correlation has not said it was good. Run a full K of them,
	 * so the verdict is being denied by the missing evidence and not merely by
	 * a consensus that has not built up yet. */
	for (unsigned i = 0; i < TU_RANGE_TRUST_K; i++) {
		tu_rs_latch_cm(100 + (int32_t)i, 60u + i);
	}
	T_OK("ig.latched", tu_rs_last_range_integrity(&ig));
	T_OK("ig.trust_built_without_evidence", tu_rs_trusted());
	T_OK("ig.no_evidence_is_not_ok", !ig.sts_ok);
	/* K good blocks earn the verdict, and not one block sooner: the evidence
	 * window is the same K that layer 4 uses for trust. */
	tu_rs_reset_ranges();
	tu_rs_set_sts(0, 200);
	tu_rs_latch_cm(100, 61u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.one_good_block_insufficient", !ig.sts_ok);
	tu_rs_set_sts(0, 150);
	tu_rs_latch_cm(101, 62u);
	tu_rs_set_sts(0, 180);
	tu_rs_latch_cm(102, 63u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.k_good_blocks_ok", ig.sts_ok);
	T_EQ("ig.quality_is_the_worst_in_the_run", ig.sts_quality, 150);
	T_EQ("ig.trust_level_reported", ig.trust_level, TU_RANGE_TRUST_K);
	/* One suspect block drops the verdict even though the distances still agree
	 * and layer 4 trust is untouched. Without this, an attacker only has to land
	 * a single spoofed block inside an otherwise clean run. */
	tu_rs_set_sts(-1, 200);
	tu_rs_latch_cm(103, 64u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.one_suspect_block_clears_it", !ig.sts_ok);
	T_OK("ig.layer4_trust_survives_it", tu_rs_trusted());
	/* ...and it heals after K clean blocks, rather than poisoning a stationary
	 * phone until the distance happens to jump. */
	tu_rs_set_sts(0, 210);
	tu_rs_latch_cm(104, 65u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.heal.not_yet", !ig.sts_ok);
	tu_rs_set_sts(0, 220);
	tu_rs_latch_cm(105, 66u);
	tu_rs_set_sts(0, 230);
	tu_rs_latch_cm(106, 67u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.heal.after_k_clean_blocks", ig.sts_ok);
	T_EQ("ig.heal.quality_forgets_the_bad_run", ig.sts_quality, 210);
	/* Evidence is consumed, never inherited: a second latch with no fresh call
	 * must not reuse the previous block's good verdict. */
	tu_rs_reset_ranges();
	tu_rs_set_sts(0, 200);
	tu_rs_latch_cm(110, 70u);
	tu_rs_latch_cm(111, 71u);
	tu_rs_latch_cm(112, 72u);
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.not_inherited", !ig.sts_ok);
	/* An implausible block clears the evidence as well as the trust. */
	tu_rs_reset_ranges();
	for (unsigned i = 0; i < TU_RANGE_TRUST_K; i++) {
		tu_rs_set_sts(0, 200);
		tu_rs_latch_cm(120 + (int32_t)i, 80u + i);
	}
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.clean_run_ok", ig.sts_ok);
	tu_rs_set_sts(0, 200);
	tu_rs_latch_cm(-500, 90u); /* Ghost-Peak: rejected by layer 1 */
	tu_rs_last_range_integrity(&ig);
	T_OK("ig.implausible_clears_evidence", !ig.sts_ok);

	t_group("range listener fires on accepted latches only");
	s_listener_hits = 0;
	tu_rs_set_range_listener(range_listener);
	tu_rs_latch_cm(100, 50u); /* accepted */
	T_EQ("listener.accepted", s_listener_hits, 1);
	tu_rs_latch_cm(-500, 51u); /* rejected (implausible) */
	T_EQ("listener.rejected_silent", s_listener_hits, 1);
	tu_rs_latch_cm(102, 52u); /* accepted again */
	T_EQ("listener.accepted_again", s_listener_hits, 2);
	tu_rs_set_range_listener(NULL); /* cleared: latches stay silent */
	tu_rs_latch_cm(103, 53u);
	T_EQ("listener.cleared", s_listener_hits, 2);
}
