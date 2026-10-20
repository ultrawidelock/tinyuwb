/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file round.c — the responder's ranging round: Pre-POLL listen, SP3 POLL window,
 * delayed Response, Final window, Final_Data decode, DS-TWR, range latch.
 *
 * The round on air, one ranging block (about 192 ms apart, 2 ms slots):
 *
 *   initiator  Pre-POLL (SP0 data, CCM* under mUPSK1)   slot 0
 *   initiator  POLL     (SP3 no-data, STS)               slot 1
 *   responder  Response (SP3 no-data, STS)               slot 2
 *   initiator  Final    (SP3 no-data, STS)               slot 3
 *   initiator  Final_Data (SP0 data, CCM* under dUDSK)   right after
 *
 * The Pre-POLL carries the POLL's STS index; the responder learns the per-block index
 * stride from two consecutive Pre-POLLs and derives the NEXT block's three STS values in
 * the idle, so no key derivation runs inside a slot. A Pre-POLL's decode is deferred past
 * the block's Final for the same reason; the Final_Data is decoded inline since it is not
 * time-critical. Timestamps t2 (POLL RX), t3 (Response TX) and t6 (Final RX) are
 * snapshotted at the Final so the Final_Data, which lands later, computes from one round.
 */

#include "tinyuwb/hw.h"
#include "tinyuwb/kdf.h"
#include "tinyuwb/mac.h"
#include "tinyuwb/range.h"
#include "tinyuwb/sts.h"
#include "tinyuwb/twr.h"
#include "tinyuwb/uwb.h"

#include <errno.h>
#include <string.h>

/* Build-time knobs. */
#ifndef TU_NUM_RESPONDERS
#define TU_NUM_RESPONDERS 1u /**< responders in the round (M3 Number_Responder_Nodes) */
#endif
#ifndef TU_RESPONDER_INDEX
#define TU_RESPONDER_INDEX 0u /**< which responder this build is */
#endif
#ifndef TU_RANGE_BIAS_MM
#define TU_RANGE_BIAS_MM 0 /**< bench calibration added to every distance */
#endif
#if TU_RESPONDER_INDEX >= TU_NUM_RESPONDERS
#error "TU_RESPONDER_INDEX must be < TU_NUM_RESPONDERS"
#endif
/** The initiator's Final sits one slot past the last responder. */
#define TU_FINAL_SLOT_OFFSET (TU_NUM_RESPONDERS + 1u)

/* The negotiated slot the initiator assumes for its single-sided ToF: M3 sends 400 *
 * chaps_per_slot RSTU (chaps=6 => 2400 RSTU), 1 RSTU = 208 hi32 ticks, so 499200 hi32. */
#define TU_SLOT_NOMINAL_HI32 499200u
/* Reader reply-path excess (TX antenna + processing), hi32 ticks, measured on the bench. */
#define TU_RESP_ANT_DLY_HI32 62u
/* Response RMARKER offset from the POLL RX: the nominal slot minus the reply-path delay. */
#define TU_RESP_SLOT_HI32 (TU_SLOT_NOMINAL_HI32 - TU_RESP_ANT_DLY_HI32)
/* One 2 ms slot as measured on air, for RX-window arming. */
#define TU_RX_SLOT_HI32 499000u
/* Open the SP3 POLL window this much before the POLL RMARKER (about 160 us). */
#define TU_RX_POLL_LEAD 40000u
/* SP3 POLL / Final RX window: about 1.4 ms. */
#define TU_RX_POLL_WIN_TO 1350u
/* Final_Data delayed SP0 window: open about 100 us after the Final, hold about 8 ms. */
#define TU_FDRX_OPEN_HI32 25000u
#define TU_FDRX_WIN_TO 7800u
#define TU_FDRX_MIN_AHEAD 50000u
/* The preamble code the SP0 Pre-POLL is heard on (the phone's M4 SYNC_Code_Index). */
#define TU_PREPOLL_CODE 9u

/* Largest SP0 frame stashed: the PHY maximum, or a full Final_Data if ever larger. */
#define TU_SP0_FINAL_DATA_MAX_LEN                                                            \
	(TU_MHR_LEN + TU_FINAL_DATA_HDR_LEN + (TU_MAX_RESPONDERS * TU_RESPONDER_LEN) +         \
	 TU_SP0_MIC_LEN + 2u)
#define TU_SP0_STASH_LEN                                                                     \
	((TU_SP0_FINAL_DATA_MAX_LEN) > 127u ? (TU_SP0_FINAL_DATA_MAX_LEN) : 127u)

static struct {
	bool listen; /* listen gate: no self-rearm once closed */
	struct tu_round_stats stats;

	/* Pre-POLL tracking */
	uint32_t poll_sts_index; /* the initiator's Poll_STS_Index from the last Pre-POLL */
	bool have_poll_index;
	uint32_t poll_stride; /* per-block index stride, learned */
	bool have_prepoll_counter;
	uint32_t prepoll_counter;
	bool have_final_counter;
	uint32_t final_counter;
	bool have_session_block;
	uint16_t session_block;

	/* Per-session constants derived once from URSK + STS_Index0 */
	bool uad_cached;
	uint8_t c_mupsk1[TU_MUPSK1_LEN];
	uint8_t c_src_long[TU_SRC_LONG_ADDR_LEN];
	uint8_t c_keysource[TU_KEYSOURCE_LEN];
	uint8_t c_dest[TU_DEST_SHORT_ADDR_LEN];

	/* STS pre-derived in the idle for the predicted next POLL, Response and Final */
	bool warm_valid;
	uint32_t warm_index;
	uint8_t warm_dursk[TU_DURSK_LEN], warm_sts_v[TU_STS_V_LEN];
	uint8_t warm_resp_dursk[TU_DURSK_LEN], warm_resp_sts_v[TU_STS_V_LEN];
	uint8_t warm_final_dursk[TU_DURSK_LEN], warm_final_sts_v[TU_STS_V_LEN];
	/* ...and committed at the POLL arm, so this block's decode can re-warm freely */
	uint8_t armed_resp_dursk[TU_DURSK_LEN], armed_resp_sts_v[TU_STS_V_LEN];
	uint8_t armed_final_dursk[TU_DURSK_LEN], armed_final_sts_v[TU_STS_V_LEN];
	uint32_t armed_index;

	/* The round in flight */
	bool await_poll, await_final;
	uint32_t poll_ip_for_final; /* POLL RMARKER (hi32), anchor for the Final window */
	uint64_t t_poll_rx, t_resp_tx, t_final_rx;
	uint32_t final_reply1, final_round2; /* snapshotted at the Final */
	bool final_round_valid;
	int32_t final_sts_verdict;
	int16_t final_sts_index;
	bool final_evidence_valid;
	uint32_t final_evidence_index, final_evidence_poll_index;

	/* Deferred Pre-POLL decode */
	uint8_t pp_stash[TU_SP0_STASH_LEN];
	uint16_t pp_stash_len;
	bool pp_pending;
} g;

/* The Response body is not radiated (SP3 no-data sends the STS only); the sequence still
 * writes a frame body. */
static const uint8_t k_resp_payload[4];

static bool counter_newer(uint32_t next, uint32_t previous)
{
	return (int32_t)(next - previous) > 0;
}

static uint32_t hi32(uint64_t ts40)
{
	return (uint32_t)(ts40 >> 8);
}

/* Both MHR fields are the byte-reverse of the UAD-derived arrays: tu_uad_addresses emits
 * them most-significant byte first, the frame carries them least-significant byte first. */
static bool mhr_context_ok(const struct tu_mhr_fields *mhr)
{
	uint16_t dest = (uint16_t)(((uint16_t)g.c_dest[0] << 8) | g.c_dest[1]);
	size_t i;

	if (mhr->dest_short_addr != dest) {
		return false;
	}
	for (i = 0u; i < sizeof(g.c_keysource); i++) {
		if (mhr->key_source[i] != g.c_keysource[sizeof(g.c_keysource) - 1u - i]) {
			return false;
		}
	}
	return true;
}

static void round_reset(void)
{
	bool listen = g.listen;

	memset(&g, 0, sizeof(g));
	g.listen = listen;
	g.final_sts_verdict = -1; /* fail-closed until a Final is measured */
}

/** Decode a Pre-POLL: verify, decrypt, learn the index and stride, warm the next block. */
static void prepoll_decode(const uint8_t *frame, uint16_t len)
{
	struct tu_mhr_fields mhr;
	struct tu_pre_poll pp;
	const uint8_t *ursk;
	uint8_t plain[TU_PRE_POLL_LEN];

	if (len < (uint16_t)(TU_MHR_LEN + TU_PRE_POLL_LEN + TU_SP0_MIC_LEN) ||
	    tu_parse_mhr(frame, &mhr) != 0 || mhr.msg_id != TU_MSG_ID_PRE_POLL ||
	    mhr.payload_len != TU_PRE_POLL_LEN ||
	    (uint16_t)(TU_MHR_LEN + mhr.payload_len + TU_SP0_MIC_LEN) > len) {
		return;
	}
	ursk = tu_rs_get_ursk();
	if (ursk == NULL) {
		return;
	}
	if (!g.uad_cached) {
		uint8_t mupsk2[TU_MUPSK2_LEN], uad[TU_UAD_LEN];

		tu_derive_mupsk1(ursk, g.c_mupsk1);
		tu_derive_mupsk2(ursk, mupsk2);
		tu_derive_uad(mupsk2, tu_sts_index0(), uad);
		tu_uad_addresses(uad, g.c_keysource, g.c_dest, g.c_src_long);
		g.uad_cached = true;
	}
	if (!mhr_context_ok(&mhr)) {
		return;
	}
	if (tu_sp0_decrypt(g.c_mupsk1, g.c_src_long, mhr.frame_counter, frame, TU_MHR_LEN,
			   &frame[TU_MHR_LEN], mhr.payload_len,
			   &frame[TU_MHR_LEN + mhr.payload_len], plain) != 0) {
		return;
	}
	tu_pre_poll_parse(plain, &pp);
	if (pp.uwb_session_id != tu_rs_id() ||
	    (g.have_prepoll_counter && !counter_newer(mhr.frame_counter, g.prepoll_counter)) ||
	    (g.have_poll_index && !counter_newer(pp.poll_sts_index, g.poll_sts_index))) {
		return;
	}
	g.prepoll_counter = mhr.frame_counter;
	g.have_prepoll_counter = true;
	g.stats.prepoll_ok++;
	g.session_block = pp.ranging_block;
	g.have_session_block = true;
	if (g.have_poll_index) {
		g.poll_stride = pp.poll_sts_index - g.poll_sts_index;
	}
	g.poll_sts_index = pp.poll_sts_index;
	g.have_poll_index = true;
	/* Warm the NEXT block's POLL, Response and Final STS in this idle: same dURSK, STS-V
	 * advances. */
	if (g.poll_stride != 0u) {
		uint32_t widx = g.poll_sts_index + g.poll_stride;
		int rc = tu_sts_for_index(widx, g.warm_dursk, g.warm_sts_v);

		if (rc == 0) {
			rc = tu_sts_for_index(widx + 1u + TU_RESPONDER_INDEX, g.warm_resp_dursk,
					      g.warm_resp_sts_v);
		}
		if (rc == 0) {
			rc = tu_sts_for_index(widx + TU_FINAL_SLOT_OFFSET, g.warm_final_dursk,
					      g.warm_final_sts_v);
		}
		if (rc == 0) {
			g.warm_index = widx;
			g.warm_valid = true;
		} else {
			g.stats.warm_fail++;
		}
	}
}

/** Decode a Final_Data: dUDSK-decrypt, parse the initiator's timestamps, DS-TWR, latch. */
static void final_data_decode(const uint8_t *frame, uint16_t len)
{
	struct tu_mhr_fields mhr;
	struct tu_final_data fd;
	struct tu_twr tw;
	uint8_t dudsk[TU_DUDSK_LEN];
	uint8_t plain[TU_FINAL_DATA_HDR_LEN + (TU_MAX_RESPONDERS * TU_RESPONDER_LEN)];

	g.stats.final_data++;
	if (tu_parse_mhr(frame, &mhr) != 0 || mhr.msg_id != TU_MSG_ID_FINAL_DATA ||
	    !g.uad_cached || !mhr_context_ok(&mhr)) {
		return;
	}
	if (mhr.payload_len > sizeof(plain) ||
	    (uint16_t)(TU_MHR_LEN + mhr.payload_len + TU_SP0_MIC_LEN) > len) {
		return;
	}
	/* Derive against the Final capture's snapshotted POLL index, never the live one. */
	if (!g.final_evidence_valid ||
	    tu_sts_dudsk_for_index(g.final_evidence_poll_index, dudsk) != 0) {
		return;
	}
	if (tu_sp0_decrypt(dudsk, g.c_src_long, mhr.frame_counter, frame, TU_MHR_LEN,
			   &frame[TU_MHR_LEN], mhr.payload_len,
			   &frame[TU_MHR_LEN + mhr.payload_len], plain) != 0) {
		return;
	}
	if (tu_final_data_parse(plain, mhr.payload_len, &fd) != 0 || fd.num_responders == 0u) {
		return;
	}
	/* Session binding, the Final's STS index, and replay protection. The 16-bit block
	 * counter is deliberately not compared: it reaches us through the deferred Pre-POLL
	 * decode and is one block behind the evidence whenever a decode is pending. */
	if (fd.uwb_session_id != tu_rs_id() || fd.final_sts_index != g.final_evidence_index ||
	    (g.have_final_counter && !counter_newer(mhr.frame_counter, g.final_counter))) {
		return;
	}
	g.final_counter = mhr.frame_counter;
	g.have_final_counter = true;

	if (g.final_round_valid &&
	    tu_responder_twr(&fd, TU_RESPONDER_INDEX, g.final_reply1, g.final_round2, &tw) == 0) {
		/* 1 tick is about 15.65 ps, 4.6917 mm. */
		int32_t tof = tu_twr_tof(&tw);
		int d_mm = (int)(((int64_t)tof * 4692) / 1000) + TU_RANGE_BIAS_MM;
		bool sts_ok = tu_rs_sts_quality_ok(g.final_sts_verdict, g.final_sts_index);

		tu_rs_set_sts(g.final_sts_verdict, g.final_sts_index);
#if defined(TU_RANGE_GATE_STRICT)
		if (sts_ok)
#else
		(void)sts_ok;
#endif
		{
			g.stats.range++;
			tu_rs_latch_cm(d_mm / 10, fd.ranging_block);
		}
	}
	/* One authenticated Final_Data consumes one Final capture, whatever DS-TWR said. */
	g.final_evidence_valid = false;
	g.final_evidence_poll_index = 0u;
	g.final_sts_verdict = -1;
	g.final_sts_index = 0;
	g.final_round_valid = false;
}

void tu_round_try_prepoll(uint16_t len)
{
	struct tu_mhr_fields m;

	if (!tu_sts_active() || len == 0u || len > sizeof(g.pp_stash)) {
		return;
	}
	if (g.pp_pending) { /* a prior block's decode never ran (missed POLL): flush it */
		g.pp_pending = false;
		prepoll_decode(g.pp_stash, g.pp_stash_len);
	}
	tu_hw_rx_read(g.pp_stash, len);
	g.pp_stash_len = len;
	if (len >= (uint16_t)TU_MHR_LEN && tu_parse_mhr(g.pp_stash, &m) == 0 &&
	    m.msg_id == TU_MSG_ID_FINAL_DATA) {
		final_data_decode(g.pp_stash, len); /* never deferred */
		return;
	}
	if (!g.warm_valid) {
		prepoll_decode(g.pp_stash, len); /* bootstrap: seed the first warm */
	} else {
		g.pp_pending = true; /* steady state: decode in the idle */
	}
}

static int gated_rx_enable(bool delayed, uint32_t at, uint32_t timeout)
{
	if (!g.listen) {
		return -ECANCELED;
	}
	return tu_hw_rx_enable(delayed, at, timeout);
}

/** Load the pre-warmed STS, flip to SP3, arm a delayed RX for the POLL one slot on. */
static int arm_poll_sp3(uint32_t prepoll_ip)
{
	if (!g.warm_valid) {
		return -EIO;
	}
	memcpy(g.armed_resp_dursk, g.warm_resp_dursk, TU_DURSK_LEN);
	memcpy(g.armed_resp_sts_v, g.warm_resp_sts_v, TU_STS_V_LEN);
	memcpy(g.armed_final_dursk, g.warm_final_dursk, TU_DURSK_LEN);
	memcpy(g.armed_final_sts_v, g.warm_final_sts_v, TU_STS_V_LEN);
	tu_hw_sts_key(g.warm_dursk);
	tu_hw_sts_iv(g.warm_sts_v);
	tu_hw_sts_mode(true);
	if (gated_rx_enable(true, prepoll_ip + TU_RX_SLOT_HI32 - TU_RX_POLL_LEAD,
			    TU_RX_POLL_WIN_TO) != 0) {
		tu_hw_sts_mode(false);
		return -EIO;
	}
	return 0;
}

/** Back to the permanent SP0 listen, no timeout. */
static void revert_to_sp0_listen(void)
{
	tu_hw_trx_off(); /* a refused delayed start leaves the sequencer pending */
	tu_hw_sts_mode(false);
	(void)gated_rx_enable(false, 0u, 0u);
}

/** A delayed SP0 window pinned to the Final_Data slot: the receiver is blind for about a
 * slot right after the Final, so an immediate re-arm misses the Final_Data. */
static int arm_final_data_sp0(uint32_t final_ip)
{
	uint32_t now;
	uint32_t dx = final_ip + TU_FDRX_OPEN_HI32;

	tu_hw_trx_off();
	tu_hw_sts_mode(false);
	now = tu_hw_time_hi32();
	if ((int32_t)(dx - now) < (int32_t)TU_FDRX_MIN_AHEAD) {
		dx = now + TU_FDRX_MIN_AHEAD;
	}
	return gated_rx_enable(true, dx, TU_FDRX_WIN_TO) == 0 ? 0 : -EIO;
}

/** Delayed-TX our Response (SP3 no-data) one slot after the POLL. */
static int tx_response_sp3(uint32_t poll_ip)
{
	uint32_t dx = poll_ip + TU_RESP_SLOT_HI32 + TU_RESPONDER_INDEX * TU_SLOT_NOMINAL_HI32;

	tu_hw_sts_key(g.armed_resp_dursk);
	tu_hw_sts_iv(g.armed_resp_sts_v);
	/* STS mode stays SP3: the Response is the same kind of frame as the POLL. */
	return tu_hw_tx_delayed(k_resp_payload, sizeof(k_resp_payload), true, dx) == 0 ? 0 : -EIO;
}

/** Arm the delayed SP3 RX for the initiator's Final. */
static int arm_final_sp3(uint32_t poll_ip)
{
	uint32_t dx = poll_ip + TU_FINAL_SLOT_OFFSET * TU_RX_SLOT_HI32;

	tu_hw_sts_key(g.armed_final_dursk);
	tu_hw_sts_iv(g.armed_final_sts_v);
	return gated_rx_enable(true, dx - TU_RX_POLL_LEAD, TU_RX_POLL_WIN_TO) == 0 ? 0 : -EIO;
}

void tu_round_tx_done(void)
{
	if (!g.listen) {
		return;
	}
	g.t_resp_tx = tu_hw_tx_timestamp(); /* t3 */
	if (arm_final_sp3(g.poll_ip_for_final) == 0) {
		g.await_final = true;
	} else {
		revert_to_sp0_listen();
	}
}

void tu_round_rx_event(uint32_t flags, uint16_t len)
{
	uint64_t ip40 = 0u;
	uint32_t ip = 0u;
	bool is_pp = false;

	if (!g.listen) {
		return;
	}
	if ((flags & TU_RX_CIA) != 0u) {
		ip40 = tu_hw_rx_timestamp();
		ip = hi32(ip40);
	}
	if ((flags & TU_RX_PHD) != 0u) {
		struct tu_mhr_fields m;
		uint8_t b[TU_MHR_LEN];

		tu_hw_rx_read(b, sizeof(b));
		/* Arm only on a cleanly received, genuine Pre-POLL. */
		is_pp = ((flags & TU_RX_FCG) != 0u && len >= (uint16_t)TU_MHR_LEN &&
			 tu_parse_mhr(b, &m) == 0 && m.msg_id == TU_MSG_ID_PRE_POLL);
	}

	if (g.await_final) {
		bool cper = (flags & TU_RX_CPER) != 0u;

		g.await_final = false;
		if (ip != 0u) {
			int16_t q = 0;
			int verdict = tu_hw_sts_quality(&q);

			g.t_final_rx = ip40; /* t6 */
			g.final_reply1 = (uint32_t)(g.t_resp_tx - g.t_poll_rx);
			g.final_round2 = (uint32_t)(g.t_final_rx - g.t_resp_tx);
			g.final_round_valid = true;
			g.final_sts_verdict = cper ? -1 : verdict;
			g.final_sts_index = q;
			g.final_evidence_index = g.armed_index + TU_FINAL_SLOT_OFFSET;
			g.final_evidence_poll_index = g.armed_index;
			g.final_evidence_valid = g.have_session_block;
		}
		if (ip == 0u || arm_final_data_sp0(ip) != 0) {
			revert_to_sp0_listen();
		}
		/* Warm the next block now, with the receiver already listening in hardware. */
		if (g.pp_pending) {
			g.pp_pending = false;
			prepoll_decode(g.pp_stash, g.pp_stash_len);
		}
	} else if (g.await_poll) {
		bool cper = (flags & TU_RX_CPER) != 0u;
		int tr = -1;

		g.await_poll = false;
		g.stats.last_poll_st = flags;
		if (!cper && ip != 0u) {
			g.stats.poll_ok++;
			g.poll_ip_for_final = ip;
			g.t_poll_rx = ip40; /* t2 */
			tr = tx_response_sp3(ip);
			if (tr == 0) {
				g.stats.resp_tx++;
			}
		} else {
			g.stats.poll_fail++;
		}
		if (tr != 0) {
			revert_to_sp0_listen();
			if (g.pp_pending) {
				g.pp_pending = false;
				prepoll_decode(g.pp_stash, g.pp_stash_len);
			}
		}
		/* else: stay SP3; tu_round_tx_done arms the Final. */
	} else if (is_pp && g.warm_valid && ip != 0u && g.warm_index != g.armed_index) {
		/* One arm per predicted index; mark before arming so a re-detected Pre-POLL
		 * cannot re-arm late. */
		g.armed_index = g.warm_index;
		if (arm_poll_sp3(ip) == 0) {
			g.stats.poll_arm++;
			g.await_poll = true;
		} else {
			g.stats.arm_fail++;
			(void)gated_rx_enable(false, 0u, 0u);
		}
	} else {
		(void)gated_rx_enable(false, 0u, 0u);
	}
}

void tu_round_rx_ok(uint32_t flags, uint16_t len)
{
	bool await = g.await_poll;

	tu_round_rx_event(flags, len);
	/* The Pre-POLL stash and decode run after the arm; the POLL event has no frame. */
	if ((flags & TU_RX_FCG) != 0u && !await) {
		tu_round_try_prepoll(len);
	}
}

bool tu_round_awaiting_poll(void)
{
	return g.await_poll;
}

void tu_round_stats_get(struct tu_round_stats *out)
{
	*out = g.stats;
}

bool tu_listening(void)
{
	return g.listen;
}

/* ── Session lifecycle ───────────────────────────────────────────────────── */

static uint32_t g_start_gen;

static int listen_start(uint8_t channel)
{
	int rc = tu_hw_listen_start(channel, TU_PREPOLL_CODE);

	if (rc != 0) {
		return rc;
	}
	round_reset();
	g.listen = true;
	return tu_hw_rx_enable(false, 0u, 0u);
}

static void listen_stop(void)
{
	memset(&g.stats, 0, sizeof(g.stats));
	if (g.listen) {
		g.listen = false; /* close the gate, then kill RX */
		tu_hw_trx_off();
	}
	/* Whether or not a listener was up: a probed part left in IDLE_PLL draws 18 mA. */
	tu_hw_sleep();
}

int tu_prewarm(uint8_t channel, uint8_t sync_code_index)
{
	(void)sync_code_index;
	return tu_hw_listen_start(channel, TU_PREPOLL_CODE);
}

int tu_start(const struct tu_cfg *c)
{
	int rc;

	if (c == NULL || c->ursk == NULL) {
		return -EINVAL;
	}
	/* Trust is session-bound: the prior session's agreeing blocks must not carry over. */
	tu_rs_reset_ranges();
	tu_rs_set_provisioned_ursk(c->ursk);
	tu_rs_set_id(c->session_id);
	if (c->ranging_config != NULL && c->rc_len > 0u) {
		rc = tu_sts_bind_from_ursk(c->ursk, c->ranging_config, c->rc_len, c->sts_index0,
					   c->slot_per_round ? c->slot_per_round : 1u);
	} else {
		rc = tu_sts_bind_from_ursk(c->ursk, c->ursk, TU_URSK_LEN, c->sts_index0,
					   c->slot_per_round ? c->slot_per_round : 1u);
	}
	if (rc != 0) {
		tu_rs_set_provisioned_ursk(NULL);
		tu_rs_set_id(0u);
		return rc;
	}
	rc = listen_start(c->channel);
	if (rc != 0) {
		tu_sts_unbind();
		tu_rs_set_provisioned_ursk(NULL);
		tu_rs_set_id(0u);
		return rc;
	}
	g_start_gen++;
	return 0;
}

void tu_stop(void)
{
	tu_sts_unbind();
	tu_rs_set_provisioned_ursk(NULL);
	tu_rs_set_id(0u);
	listen_stop();
}

/* ── Range accessors ─────────────────────────────────────────────────────── */

bool tu_last_range_cm(int32_t *cm_out)
{
	return tu_rs_last_range(cm_out, NULL, NULL, NULL, NULL);
}

bool tu_last_range_age_cm(int32_t *cm_out, int64_t *age_ms_out)
{
	return tu_rs_last_range(cm_out, NULL, NULL, NULL, age_ms_out);
}

bool tu_trusted_range_cm(int32_t *cm_out)
{
	return tu_trusted_range_age_cm(cm_out, NULL);
}

bool tu_trusted_range_age_cm(int32_t *cm_out, int64_t *age_ms_out)
{
	return tu_rs_last_range(cm_out, NULL, NULL, NULL, age_ms_out) && tu_rs_trusted();
}

bool tu_trusted_range_block_cm(int32_t *cm_out, uint32_t *block_out)
{
	return tu_rs_last_range(cm_out, NULL, NULL, block_out, NULL) && tu_rs_trusted();
}

uint32_t tu_session_id(void)
{
	return tu_rs_id();
}

uint32_t tu_start_generation(void)
{
	return g_start_gen;
}

uint32_t tu_range_generation(void)
{
	return tu_rs_generation();
}

bool tu_trusted_range_after_cm(int32_t *cm_out, uint32_t after)
{
	uint32_t generation = tu_rs_generation();

	return (int32_t)(generation - after) > 0 && tu_trusted_range_cm(cm_out);
}

bool tu_trusted_range_after_checked_cm(int32_t *cm_out, uint32_t after,
				       struct tu_uwb_range_integrity *ig_out)
{
	struct tu_rs_integrity ig;

	if (ig_out != NULL) {
		ig_out->sts_ok = false;
		ig_out->sts_quality = 0;
		ig_out->trust_level = 0u;
	}
	if (!tu_trusted_range_after_cm(cm_out, after)) {
		return false;
	}
	if (ig_out != NULL && tu_rs_last_range_integrity(&ig)) {
		ig_out->sts_ok = ig.sts_ok;
		ig_out->sts_quality = ig.sts_quality;
		ig_out->trust_level = ig.trust_level;
	}
	return true;
}

void tu_set_range_listener(void (*cb)(void))
{
	tu_rs_set_range_listener(cb);
}
