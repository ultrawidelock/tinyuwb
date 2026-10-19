/**
 * @file twr.h — the double-sided two-way ranging estimator, one definition.
 *
 * Lives at the base base tier rather than in the CCC MAC because two
 * unrelated callers need it: the credential responder, which ranges a phone, and the
 * anchor-to-anchor bench link, which ranges another board and has no CCC engine
 * in its image at all.
 *
 * THE RESULT IS SIGNED, and that is the entire reason this file exists. The
 * numerator is a difference of two products, and at short range the round
 * product is the smaller of the two -- so an unsigned accumulator underflows to
 * ~1.8e19 and the divide returns a garbage distance instead of a small negative
 * one. An earlier `uint32_t tu_ds_twr_tof()` had exactly that flaw; it was
 * never called in production (the responder open-coded a signed version to
 * dodge it) and has been replaced by this.
 */

#ifndef TINYUWB_TWR_H
#define TINYUWB_TWR_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief The four DS-TWR intervals, in ranging-timestamp ticks (wrap mod 2^32).
 *
 * All four are differences of a 40-bit chip counter truncated to 32 bits, so
 * the wrap is deliberate: every interval is far shorter than the 2^32-tick
 * period, and the subtraction that produced it is correct across a wrap.
 */
struct tu_twr {
	uint32_t t_round1; /**< initiator POLL tx -> RESPONSE rx (t4 - t1) */
	uint32_t t_reply1; /**< responder POLL rx -> RESPONSE tx (t3 - t2) */
	uint32_t t_round2; /**< responder RESPONSE tx -> FINAL rx (t6 - t3) */
	uint32_t t_reply2; /**< initiator RESPONSE rx -> FINAL tx (t5 - t4) */
};

/**
 * @brief One-way time of flight, in ranging-timestamp ticks.
 *
 * The asymmetric four-term estimator, which is what makes DS-TWR tolerant of
 * the two ends' clocks running at slightly different rates:
 *
 *     tof = (round1 * round2 - reply1 * reply2)
 *           / (round1 + round2 + reply1 + reply2)
 *
 * @param t The four intervals; NULL yields 0.
 * @return Time of flight in ticks. NEGATIVE near contact, where measurement
 *         noise exceeds the true flight time -- callers converting to a
 *         distance must keep the sign and apply their own plausibility floor
 *         rather than clamping here, because "slightly negative" and "wildly
 *         wrong" want different responses. 0 if the denominator is 0.
 */
int32_t tu_twr_tof(const struct tu_twr *t);

#endif /* TINYUWB_TWR_H */
