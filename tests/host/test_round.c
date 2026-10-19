/** @file test_round.c — one full DS-TWR round through the real engine: genuinely
 * CCM*-encrypted Pre-POLLs are decoded (bootstrap + stride learning + STS warm), the SP3
 * POLL window is armed off the warm, the POLL result fires the Response TX, TX-done arms
 * the Final, and the Final_Data decrypt latches a range. Frames are built with the same
 * codec + KDF the initiator side uses, so the decrypt is real. */
#include <errno.h>
#include <string.h>

#include "hw_fake.h"
#include "test.h"
#include "tinyuwb/hw.h"
#include "tinyuwb/kdf.h"
#include "tinyuwb/mac.h"
#include "tinyuwb/range.h"
#include "tinyuwb/sts.h"
#include "tinyuwb/uwb.h"

extern int aes_ref_fail_bits; /* port/host/aes.c: fail AES blocks with keys of this size */

#define RND_SID    0x11223344u
#define RND_STS0   0x00400000u
#define RND_IDX1   5000u /* first Pre-POLL's Poll_STS_Index */
#define RND_STRIDE 96u   /* per-block index stride the decode must learn */
#define RND_BLOCK  7u

#define ST_GOOD (TU_RX_CIA | TU_RX_PHD | TU_RX_FCG)

static uint8_t g_ursk[TU_URSK_LEN];
static uint8_t g_mupsk1[TU_MUPSK1_LEN];
static uint8_t g_ks[TU_KEYSOURCE_LEN];
static uint8_t g_dest[TU_DEST_SHORT_ADDR_LEN];
static uint8_t g_src_long[TU_SRC_LONG_ADDR_LEN];

/* The Aux Security Header carries KeySource in transmission order, the byte-reverse of
 * the KeySourceHigh||KeySourceLow layout tu_uad_addresses() returns. */
static void mhr_set_keysource(uint8_t dst[TU_KEYSOURCE_LEN])
{
	for (size_t i = 0u; i < TU_KEYSOURCE_LEN; i++) {
		dst[i] = g_ks[TU_KEYSOURCE_LEN - 1u - i];
	}
}

static uint16_t mk_prepoll_for(uint8_t *out, uint32_t fc, uint32_t poll_idx, uint16_t block)
{
	struct tu_mhr_fields f;
	struct tu_pre_poll pp;
	uint8_t plain[TU_PRE_POLL_LEN];

	memset(&pp, 0, sizeof(pp));
	pp.uwb_session_id = RND_SID;
	pp.poll_sts_index = poll_idx;
	pp.ranging_block = block;
	tu_pre_poll_pack(&pp, plain);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
	f.frame_counter = fc;
	mhr_set_keysource(f.key_source);
	f.msg_id = TU_MSG_ID_PRE_POLL;
	f.payload_len = TU_PRE_POLL_LEN;
	T_EQ("mk_pp.mhr", tu_build_mhr(&f, out), 0);
	T_EQ("mk_pp.enc",
	     tu_sp0_encrypt(g_mupsk1, g_src_long, fc, out, TU_MHR_LEN, plain, TU_PRE_POLL_LEN,
			    &out[TU_MHR_LEN], &out[TU_MHR_LEN + TU_PRE_POLL_LEN]),
	     0);
	return TU_MHR_LEN + TU_PRE_POLL_LEN + TU_SP0_MIC_LEN;
}

static uint16_t mk_prepoll(uint8_t *out, uint32_t fc, uint32_t poll_idx)
{
	return mk_prepoll_for(out, fc, poll_idx, RND_BLOCK);
}

struct fd_rec {
	uint8_t idx;
	uint32_t ts;
};

/** Build an encrypted Final_Data with @p n records keyed on the armed POLL index; returns
 * the length the driver reports, FCS included. */
static uint16_t mk_final_data_n(uint8_t *out, uint32_t fc, uint32_t armed_idx,
				const struct fd_rec *recs, uint8_t n, uint32_t final_tx,
				uint32_t session_id, uint16_t block, uint32_t final_sts_index)
{
	struct tu_mhr_fields f;
	struct tu_final_data fd;
	uint8_t plain[TU_FINAL_DATA_HDR_LEN + (TU_MAX_RESPONDERS * TU_RESPONDER_LEN)];
	uint8_t dudsk[TU_DUDSK_LEN];
	size_t pl = 0;

	memset(&fd, 0, sizeof(fd));
	fd.uwb_session_id = session_id;
	fd.ranging_block = block;
	fd.final_sts_index = final_sts_index;
	fd.ranging_ts_final_tx = final_tx; /* t5-t1 */
	fd.num_responders = n;
	for (uint8_t i = 0u; i < n; i++) {
		fd.responders[i].responder_index = recs[i].idx;
		fd.responders[i].timestamp = recs[i].ts; /* t4-t1 */
	}
	T_EQ("mk_fd.pack", tu_final_data_pack(&fd, plain, sizeof(plain), &pl), 0);

	memset(&f, 0, sizeof(f));
	f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
	f.frame_counter = fc;
	mhr_set_keysource(f.key_source);
	f.msg_id = TU_MSG_ID_FINAL_DATA;
	f.payload_len = (uint8_t)pl;
	T_EQ("mk_fd.mhr", tu_build_mhr(&f, out), 0);
	T_EQ("mk_fd.key", tu_sts_dudsk_for_index(armed_idx, dudsk), 0);
	T_EQ("mk_fd.enc",
	     tu_sp0_encrypt(dudsk, g_src_long, fc, out, TU_MHR_LEN, plain, pl, &out[TU_MHR_LEN],
			    &out[TU_MHR_LEN + pl]),
	     0);
	out[TU_MHR_LEN + pl + TU_SP0_MIC_LEN] = 0xAAu;
	out[TU_MHR_LEN + pl + TU_SP0_MIC_LEN + 1u] = 0x55u;
	return (uint16_t)(TU_MHR_LEN + pl + TU_SP0_MIC_LEN + 2u);
}

static uint16_t mk_final_data_for(uint8_t *out, uint32_t fc, uint32_t armed_idx,
				  uint32_t t_round1, uint32_t t_reply2, uint32_t session_id,
				  uint16_t block, uint32_t final_sts_index)
{
	const struct fd_rec rec = {.idx = 0u, .ts = t_round1};

	return mk_final_data_n(out, fc, armed_idx, &rec, 1u, t_round1 + t_reply2, session_id,
			       block, final_sts_index);
}

static uint16_t mk_final_data(uint8_t *out, uint32_t fc, uint32_t armed_idx, uint32_t t_round1,
			      uint32_t t_reply2)
{
	return mk_final_data_for(out, fc, armed_idx, t_round1, t_reply2, RND_SID, RND_BLOCK,
				 armed_idx + 2u);
}

static void stash_frame(const uint8_t *frame, uint16_t len, uint64_t ip40)
{
	memcpy(hw.rxdata, frame, len);
	hw.rxdata_len = len;
	hw.rx_ts40 = ip40;
}

void test_round(void)
{
	uint8_t frame[128];
	uint16_t len;
	uint8_t rc[17];
	struct tu_cfg c;
	uint8_t mupsk2[TU_MUPSK2_LEN], uad[TU_UAD_LEN];
	const uint32_t widx = RND_IDX1 + 2u * RND_STRIDE; /* warmed POLL index */
	uint32_t fc = 100u;
	int32_t cm = -1;

	t_group("session setup mirrors the initiator's derivations");
	for (size_t i = 0; i < sizeof(g_ursk); i++) {
		g_ursk[i] = (uint8_t)(0xA0u + i);
	}
	for (size_t i = 0; i < sizeof(rc); i++) {
		rc[i] = (uint8_t)i;
	}
	T_EQ("kdf.mupsk1", tu_derive_mupsk1(g_ursk, g_mupsk1), 0);
	T_EQ("kdf.mupsk2", tu_derive_mupsk2(g_ursk, mupsk2), 0);
	T_EQ("kdf.uad", tu_derive_uad(mupsk2, RND_STS0, uad), 0);
	T_EQ("kdf.addr", tu_uad_addresses(uad, g_ks, g_dest, g_src_long), 0);

	memset(&c, 0, sizeof(c));
	c.session_id = RND_SID;
	c.channel = 9u;
	c.sync_code_index = 9u;
	c.slot_per_round = 12u;
	c.sts_index0 = RND_STS0;
	c.ursk = g_ursk;
	c.ranging_config = rc;
	c.rc_len = sizeof(rc);
	hw_fake_reset();
	T_EQ("start", tu_start(&c), 0);
	T_EQ("start.armed", hw.rx_enable_calls, 1);
	T_OK("start.immediate", !hw.last_rx_delayed);

	t_group("bootstrap: two Pre-POLL decodes learn index + stride");
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x1000000ull);
	tu_round_try_prepoll(len); /* inline decode #1: index, no stride */
	len = mk_prepoll(frame, fc++, RND_IDX1 + RND_STRIDE);
	stash_frame(frame, len, 0x2000000ull);
	tu_round_try_prepoll(len); /* inline decode #2: stride, warms widx */

	t_group("Pre-POLL event arms the SP3 POLL window off the warm");
	T_OK("prearm.not_awaiting", !tu_round_awaiting_poll());
	stash_frame(frame, len, 0x3000000ull); /* MHR re-read by the handler */
	tu_round_rx_event(ST_GOOD, len);
	T_OK("arm.awaiting_poll", tu_round_awaiting_poll());
	T_OK("arm.delayed", hw.last_rx_delayed);
	T_EQ("arm.at", hw.last_rx_at, 0x30000u + 499000u - 40000u);
	T_EQ("arm.timeout", hw.last_rx_timeout, 1350u);
	T_OK("arm.sp3", hw.sts_sp3);
	/* Next block's Pre-POLL arrives while armed: stash + defer its decode. */
	len = mk_prepoll(frame, fc++, RND_IDX1 + 2u * RND_STRIDE);
	stash_frame(frame, len, 0x3100000ull);
	tu_round_try_prepoll(len);

	t_group("POLL result (STS ok) fires the delayed Response TX");
	hw.rx_ts40 = 0x40000000ull; /* t2: POLL RX */
	tu_round_rx_event(TU_RX_CIA, 0u);
	T_EQ("poll.resp_tx", hw.tx_calls, 1);
	T_EQ("poll.resp_at", hw.last_tx_at, 0x400000u + 499200u - 62u);
	T_OK("poll.await_cleared", !tu_round_awaiting_poll());

	t_group("TX-done arms the Final window");
	hw.tx_ts40 = 0x40000000ull + 100000u; /* t3 = t2 + 100k DTU */
	tu_round_tx_done();
	T_OK("final.armed", hw.last_rx_delayed);
	T_EQ("final.at", hw.last_rx_at, 0x400000u + 2u * 499000u - 40000u);

	t_group("Final result stashes the STS verdict, arms the Final_Data window");
	hw.rx_ts40 = 0x40000000ull + 300000u; /* t6 = t3 + 200k DTU */
	hw.stsq_ret = 0;
	hw.stsq_val = 100;
	hw.systime = 0x400000u + 300000u / 256u;
	tu_round_rx_event(TU_RX_CIA, 0u);
	T_OK("final.sp0", !hw.sts_sp3);
	T_OK("final.fd_window.delayed", hw.last_rx_delayed);
	T_EQ("final.fd_window.timeout", hw.last_rx_timeout, 7800u);

	/* Advance the live Pre-POLL context before Final_Data is dispatched. The accepted
	 * Final must stay bound to its capture-time block, not to later globals. */
	len = mk_prepoll_for(frame, fc++, RND_IDX1 + 3u * RND_STRIDE, RND_BLOCK + 1u);
	stash_frame(frame, len, 0x3110000ull);
	tu_round_try_prepoll(len);
	len = mk_prepoll_for(frame, fc++, RND_IDX1 + 4u * RND_STRIDE, RND_BLOCK + 2u);
	stash_frame(frame, len, 0x3120000ull);
	tu_round_try_prepoll(len);

	t_group("Final_Data decrypt latches the DS-TWR range");
	/* reply1=100k, round2=200k (injected above); round1=101k, reply2=199k
	 * => tof = (101k*200k - 100k*199k) / 600k = 500 ticks = 234 cm. */
	len = mk_final_data_for(frame, fc++, widx, 101000u, 199000u, RND_SID ^ 1u, RND_BLOCK,
				widx + 2u);
	stash_frame(frame, len, 0x3180000ull);
	tu_round_try_prepoll(len);
	T_OK("wrong-session Final_Data rejected",
	     !tu_rs_last_range(&cm, NULL, NULL, NULL, NULL));

	len = mk_final_data(frame, fc++, widx, 101000u, 199000u);
	stash_frame(frame, len, 0x3200000ull);
	tu_round_try_prepoll(len); /* Final_Data decodes inline */
	T_OK("range.latched", tu_rs_last_range(&cm, NULL, NULL, NULL, NULL));
	T_EQ("range.cm", cm, 234);
	{
		struct tu_round_stats st;

		tu_round_stats_get(&st);
		T_EQ("stats.prepoll_ok", st.prepoll_ok, 5);
		T_EQ("stats.poll_arm", st.poll_arm, 1);
		T_EQ("stats.poll_ok", st.poll_ok, 1);
		T_EQ("stats.poll_fail", st.poll_fail, 0);
		T_EQ("stats.resp_tx", st.resp_tx, 1);
		T_EQ("stats.final_data", st.final_data, 2);
		T_EQ("stats.range", st.range, 1);
	}
	{
		uint32_t generation = tu_rs_generation();

		stash_frame(frame, len, 0x3210000ull);
		tu_round_try_prepoll(len);
		T_EQ("replayed Final_Data cannot relatch", (long)tu_rs_generation(),
		     (long)generation);
	}

	t_group("two-record Final_Data survives the SP0 gate and selects by tag");
	{
		const struct fd_rec recs[2] = {
			{.idx = 1u, .ts = 150000u}, /* another responder's, first in the array */
			{.idx = 0u, .ts = 101000u}, /* ours, second */
		};

		len = mk_final_data_n(frame, fc++, widx, recs, 2u, 300000u, RND_SID, RND_BLOCK,
				      widx + 2u);
		T_EQ("two-record frame is 65 bytes on air", (long)len, 65L);
		stash_frame(frame, len, 0x3220000ull);
		tu_round_try_prepoll(len);
		T_OK("two-record range latched", tu_rs_last_range(&cm, NULL, NULL, NULL, NULL));
		T_EQ("two-record range picks the tag-0 record", cm, 234);
	}

	t_group("round 2: POLL result with STS error reverts and reflushes");
	len = mk_prepoll(frame, fc++, RND_IDX1 + 5u * RND_STRIDE);
	stash_frame(frame, len, 0x4000000ull);
	tu_round_rx_event(ST_GOOD, len); /* re-arm off decode #3's warm */
	T_OK("arm2.awaiting", tu_round_awaiting_poll());
	len = mk_prepoll(frame, fc++, RND_IDX1 + 6u * RND_STRIDE);
	stash_frame(frame, len, 0x4100000ull);
	tu_round_try_prepoll(len); /* pending decode #4 */
	tu_round_rx_event(TU_RX_CIA | TU_RX_CPER, 0u);
	T_OK("poll2.no_tx", hw.tx_calls == 1); /* no new Response */
	T_OK("poll2.sp0", !hw.last_rx_delayed);
	T_OK("poll2.sp0_mode", !hw.sts_sp3);

	t_group("a refused delayed arm falls back to the SP0 listen");
	stash_frame(frame, len, 0x5000000ull);
	hw.rx_enable_ret = -ETIME;
	tu_round_rx_event(ST_GOOD, len); /* arm fails */
	T_OK("armfail.not_awaiting", !tu_round_awaiting_poll());
	T_OK("armfail.sp0_mode", !hw.sts_sp3);
	hw.rx_enable_ret = 0;

	t_group("restart: a Pre-POLL event with no warm cannot arm SP3");
	tu_stop();
	T_EQ("restart", tu_start(&c), 0); /* the reset clears the warm */
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x6000000ull);
	tu_round_rx_event(ST_GOOD, len);
	T_OK("nowarm.not_awaiting", !tu_round_awaiting_poll());

	t_group("prepoll decode rejects every malformed frame class");
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x6100000ull);
	tu_round_try_prepoll((uint16_t)(TU_MHR_LEN + 2u)); /* too short */
	len = mk_prepoll(frame, fc++, RND_IDX1);
	frame[0] ^= 0xFFu; /* corrupted frame control */
	stash_frame(frame, len, 0x6200000ull);
	tu_round_try_prepoll(len);
	{
		struct tu_mhr_fields f;

		memset(&f, 0, sizeof(f));
		f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
		f.frame_counter = fc++;
		mhr_set_keysource(f.key_source);
		f.msg_id = 0x07u; /* neither Pre-POLL nor Final */
		f.payload_len = TU_PRE_POLL_LEN;
		memset(frame, 0, sizeof(frame));
		T_EQ("mk_odd.mhr", tu_build_mhr(&f, frame), 0);
		len = TU_MHR_LEN + TU_PRE_POLL_LEN + TU_SP0_MIC_LEN;
		stash_frame(frame, len, 0x6300000ull);
		tu_round_try_prepoll(len);
		f.msg_id = TU_MSG_ID_PRE_POLL;
		f.payload_len = TU_PRE_POLL_LEN - 1u; /* wrong payload length */
		T_EQ("mk_badlen.mhr", tu_build_mhr(&f, frame), 0);
		stash_frame(frame, len, 0x6400000ull);
		tu_round_try_prepoll(len);
	}
	tu_rs_set_provisioned_ursk(NULL); /* no URSK: the decode stops early */
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x6500000ull);
	tu_round_try_prepoll(len);
	tu_rs_set_provisioned_ursk(g_ursk);
	len = mk_prepoll(frame, fc++, RND_IDX1);
	frame[len - 1u] ^= 0x01u; /* flipped MIC byte */
	stash_frame(frame, len, 0x6600000ull);
	tu_round_try_prepoll(len);
	T_OK("badframes.no_warm", !tu_round_awaiting_poll());

	t_group("Final_Data decode rejects malformed frames");
	{
		struct tu_mhr_fields f;

		memset(&f, 0, sizeof(f));
		f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
		f.frame_counter = fc++;
		mhr_set_keysource(f.key_source);
		f.msg_id = TU_MSG_ID_FINAL_DATA;
		f.payload_len = 100u; /* beyond the plaintext scratch */
		memset(frame, 0, sizeof(frame));
		T_EQ("mk_fdbig.mhr", tu_build_mhr(&f, frame), 0);
		stash_frame(frame, 40u, 0x6700000ull);
		tu_round_try_prepoll(40u);
	}
	len = mk_final_data(frame, fc++, 0u, 101000u, 199000u);
	frame[len - 1u] ^= 0x01u; /* flipped MIC byte */
	stash_frame(frame, len, 0x6800000ull);
	tu_round_try_prepoll(len);
	{
		struct tu_mhr_fields f;
		uint8_t plain[18];
		uint8_t dudsk[TU_DUDSK_LEN];

		memset(plain, 0, sizeof(plain)); /* zero responders reported */
		memset(&f, 0, sizeof(f));
		f.dest_short_addr = (uint16_t)(((uint16_t)g_dest[0] << 8) | g_dest[1]);
		f.frame_counter = fc;
		mhr_set_keysource(f.key_source);
		f.msg_id = TU_MSG_ID_FINAL_DATA;
		f.payload_len = sizeof(plain);
		T_EQ("mk_fd0.mhr", tu_build_mhr(&f, frame), 0);
		T_EQ("mk_fd0.key", tu_sts_dudsk_for_index(0u, dudsk), 0);
		T_EQ("mk_fd0.enc",
		     tu_sp0_encrypt(dudsk, g_src_long, fc, frame, TU_MHR_LEN, plain, sizeof(plain),
				    &frame[TU_MHR_LEN], &frame[TU_MHR_LEN + sizeof(plain)]),
		     0);
		fc++;
		len = TU_MHR_LEN + sizeof(plain) + TU_SP0_MIC_LEN;
		stash_frame(frame, len, 0x6900000ull);
		tu_round_try_prepoll(len);
	}
	T_OK("badfinal.survived", 1);

	t_group("an orphaned pending decode is flushed by the next Pre-POLL");
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x7000000ull);
	tu_round_try_prepoll(len); /* bootstrap decode #1 */
	len = mk_prepoll(frame, fc++, RND_IDX1 + RND_STRIDE);
	stash_frame(frame, len, 0x7100000ull);
	tu_round_try_prepoll(len); /* decode #2: stride learned, warm valid */
	len = mk_prepoll(frame, fc++, RND_IDX1 + 2u * RND_STRIDE);
	stash_frame(frame, len, 0x7200000ull);
	tu_round_try_prepoll(len); /* steady state: deferred */
	len = mk_prepoll(frame, fc++, RND_IDX1 + 3u * RND_STRIDE);
	stash_frame(frame, len, 0x7300000ull);
	tu_round_try_prepoll(len); /* missed POLL: flushes the orphan first */
	T_OK("flush.survived", 1);

	t_group("try_prepoll is gated once the listener is stopped");
	tu_stop();
	tu_round_try_prepoll(len);
	T_OK("stopped.not_awaiting", !tu_round_awaiting_poll());
	{
		struct tu_round_stats st;

		tu_round_stats_get(&st);
		T_EQ("stats.zeroed_by_stop", st.prepoll_ok + st.poll_arm + st.range, 0);
	}

	t_group("a failed STS warm is counted, and nothing is armed off it");
	hw_fake_reset();
	T_EQ("warm.start", tu_start(&c), 0);
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x8000000ull);
	tu_round_try_prepoll(len); /* index only: no warm attempted */
	len = mk_prepoll(frame, fc++, RND_IDX1 + RND_STRIDE);
	stash_frame(frame, len, 0x8100000ull);
	aes_ref_fail_bits = 256;
	tu_round_try_prepoll(len); /* stride known: the warm runs and fails */
	aes_ref_fail_bits = 0;
	tu_round_rx_event(ST_GOOD, len);
	T_OK("warm.no_arm", !tu_round_awaiting_poll());
	{
		struct tu_round_stats st;

		tu_round_stats_get(&st);
		T_EQ("warm.counted", st.warm_fail, 1);
		T_EQ("warm.no_arm_counted", st.poll_arm, 0);
	}

	t_group("a refused POLL arm is counted");
	len = mk_prepoll(frame, fc++, RND_IDX1 + 2u * RND_STRIDE);
	stash_frame(frame, len, 0x8200000ull);
	tu_round_try_prepoll(len); /* warm succeeds this time */
	hw.rx_enable_ret = -ETIME;
	stash_frame(frame, len, 0x8300000ull);
	tu_round_rx_event(ST_GOOD, len);
	hw.rx_enable_ret = 0;
	T_OK("arm.refused_not_awaiting", !tu_round_awaiting_poll());
	{
		struct tu_round_stats st;

		tu_round_stats_get(&st);
		T_EQ("arm.fail_counted", st.arm_fail, 1);
		T_EQ("arm.none_armed", st.poll_arm, 0);
	}

	t_group("the driver's composed RX-ok path arms, then decodes");
	tu_stop();
	hw_fake_reset();
	T_EQ("ok.start", tu_start(&c), 0);
	len = mk_prepoll(frame, fc++, RND_IDX1);
	stash_frame(frame, len, 0x9000000ull);
	tu_round_rx_ok(ST_GOOD, len); /* bootstrap decode through the composed path */
	len = mk_prepoll(frame, fc++, RND_IDX1 + RND_STRIDE);
	stash_frame(frame, len, 0x9100000ull);
	tu_round_rx_ok(ST_GOOD, len); /* stride learned, warm valid */
	len = mk_prepoll(frame, fc++, RND_IDX1 + 2u * RND_STRIDE);
	stash_frame(frame, len, 0x9200000ull);
	tu_round_rx_ok(ST_GOOD, len); /* arms the POLL window, defers the decode */
	T_OK("ok.awaiting_poll", tu_round_awaiting_poll());
	tu_stop();
}
