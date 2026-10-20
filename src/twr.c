/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/**
 * @file twr.c — the double-sided two-way ranging estimator (implementation).
 */

#include "tinyuwb/twr.h"

int32_t tu_twr_tof(const struct tu_twr *t)
{
	int64_t num, den;

	if (t == NULL) {
		return 0;
	}
	/*
	 * Each product is at most 2^64 in principle but far smaller in practice:
	 * the intervals are one ranging round apart, a few milliseconds at
	 * 15.65 ps per tick, so each fits comfortably inside 2^32 and the
	 * product inside 2^64. Computed as uint64_t and then taken as signed so
	 * the two products are formed without overflow and only their
	 * DIFFERENCE carries a sign -- which is the one place the sign matters.
	 */
	num = (int64_t)((uint64_t)t->t_round1 * t->t_round2) -
	      (int64_t)((uint64_t)t->t_reply1 * t->t_reply2);
	den = (int64_t)t->t_round1 + t->t_round2 + t->t_reply1 + t->t_reply2;

	return den != 0 ? (int32_t)(num / den) : 0;
}
