/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file test_setup.c — the Ranging Service: M1/M3 builders, M2/M4 handling, suspend and
 * resume, notifications, and the radio start at M4. */
#include <string.h>

#include "hw_fake.h"
#include "test.h"
#include "tinyuwb/setup.h"
#include "tinyuwb/uwb.h"

/* --- transmit + event capture ------------------------------------------- */

static struct {
	uint8_t msg[64];
	size_t len;
	int count;
} g_tx;

static struct {
	int count;
	enum tu_setup_event last;
} g_ev;

static void tx_cb(const uint8_t *m, size_t len, void *user)
{
	(void)user;
	memcpy(g_tx.msg, m, len);
	g_tx.len = len;
	g_tx.count++;
}

static void ev_cb(enum tu_setup_event ev, void *user)
{
	(void)user;
	g_ev.count++;
	g_ev.last = ev;
}

/* --- message helpers ---------------------------------------------------- */

struct mb {
	uint8_t buf[160];
	size_t len;
};

static struct mb mk(uint8_t proto, uint8_t id)
{
	struct mb b;

	memset(&b, 0, sizeof(b));
	b.buf[0] = proto;
	b.buf[1] = id;
	b.len = 4u;
	return b;
}

static void add(struct mb *b, uint8_t id, const uint8_t *v, uint8_t n)
{
	b->buf[b->len++] = id;
	b->buf[b->len++] = n;
	if (n > 0u && v != NULL) {
		memcpy(&b->buf[b->len], v, n);
	}
	b->len += n;
}

static void add_u8(struct mb *b, uint8_t id, uint8_t v)
{
	add(b, id, &v, 1u);
}

static void add_u16(struct mb *b, uint8_t id, uint16_t v)
{
	uint8_t x[2] = {(uint8_t)(v >> 8), (uint8_t)v};

	add(b, id, x, 2u);
}

static void add_u32(struct mb *b, uint8_t id, uint32_t v)
{
	uint8_t x[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};

	add(b, id, x, 4u);
}

static void add_u64(struct mb *b, uint8_t id, uint64_t v)
{
	uint8_t x[8];

	for (int i = 0; i < 8; i++) {
		x[i] = (uint8_t)(v >> (56 - 8 * i));
	}
	add(b, id, x, 8u);
}

/** Fix the payload length and hand the message in. */
static enum tu_setup_err feed(struct mb *b)
{
	uint16_t plen = (uint16_t)(b->len - 4u);

	b->buf[2] = (uint8_t)(plen >> 8);
	b->buf[3] = (uint8_t)plen;
	return tu_setup_handle(b->buf, b->len);
}

static void add_m2_attrs(struct mb *b)
{
	add_u16(b, TU_RS_ATTR_CONFIGURATION_IDENTIFIER, 0x0001u);
	add_u8(b, TU_RS_ATTR_PULSE_SHAPE_COMBO, 0x00u);
	add_u8(b, TU_RS_ATTR_CHANNEL_BITMASK, 0x01u);
	add_u8(b, TU_RS_ATTR_RAN_MULTIPLIER, 4u);
	add_u8(b, TU_RS_ATTR_SLOT_BITMASK, 0x01u);
	add_u32(b, TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK, 0x05u);
	add_u8(b, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0xFFu);
}

static void add_m4_attrs(struct mb *b)
{
	add_u32(b, TU_RS_ATTR_STS_INDEX0, 0x1000u);
	add_u64(b, TU_RS_ATTR_UWB_TIME0, 0u);
	add_u32(b, TU_RS_ATTR_HOP_MODE_KEY, 0x11223344u);
	add_u8(b, TU_RS_ATTR_SYNC_CODE_INDEX, 9u);
}

/* Read the first attribute with @id from the last transmitted message. */
static const uint8_t *tx_find(uint8_t id, uint8_t *len)
{
	size_t off = 4u;

	while (off + 2u <= g_tx.len) {
		uint8_t aid = g_tx.msg[off];
		uint8_t alen = g_tx.msg[off + 1u];

		if (aid == id) {
			*len = alen;
			return &g_tx.msg[off + 2u];
		}
		off += 2u + alen;
	}
	return NULL;
}

static uint32_t tx_u32(uint8_t id, uint32_t fail)
{
	uint8_t n;
	const uint8_t *v = tx_find(id, &n);

	if (v == NULL || n != 4u) {
		return fail;
	}
	return ((uint32_t)v[0] << 24) | ((uint32_t)v[1] << 16) | ((uint32_t)v[2] << 8) | v[3];
}

static uint8_t tx_u8(uint8_t id, uint8_t fail)
{
	uint8_t n;
	const uint8_t *v = tx_find(id, &n);

	return (v != NULL && n == 1u) ? v[0] : fail;
}

void test_setup(void)
{
	const uint32_t SID = 0x11223344u;
	uint8_t ursk[32];
	struct tu_setup_caps caps;
	struct mb b;

	memset(&g_tx, 0, sizeof(g_tx));
	memset(&g_ev, 0, sizeof(g_ev));
	for (size_t i = 0; i < sizeof(ursk); i++) {
		ursk[i] = (uint8_t)(i + 1u);
	}
	memset(&caps, 0, sizeof(caps));
	caps.uwb_config_id = 0x0001u;
	caps.pulse_shape_combo = 0x00u;
	caps.min_ran_multiplier = 8u;
	caps.slot_bitmask = 0x01u;
	caps.channel_bitmask = 0x01u;
	caps.hopping_config_bitmask = 0x0Au;
	caps.sync_code_index_bitmask = 0x00000005u;
	caps.hop_pref_count = 1u;
	caps.hop_prefs[0] = TU_HOP_PREF_CONTINUOUS_DEFAULT;

	t_group("open rejects null args, then opens");
	hw_fake_reset();
	T_OK("open.null.caps", tu_setup_open(NULL, SID, ursk, tx_cb, ev_cb, NULL) != 0);
	T_OK("open.null.ursk", tu_setup_open(&caps, SID, NULL, tx_cb, ev_cb, NULL) != 0);
	T_OK("open.null.tx", tu_setup_open(&caps, SID, ursk, NULL, ev_cb, NULL) != 0);
	T_OK("closed.handle", tu_setup_handle((const uint8_t *)"\x01\x00\x00\x00", 4u) ==
				      TU_SETUP_EINVAL);
	T_EQ("open.ok", tu_setup_open(&caps, SID, ursk, tx_cb, ev_cb, NULL), 0);
	T_OK("open.is_open", tu_setup_is_open());
	T_EQ("open.state", tu_setup_state(), TU_SETUP_CREATED);

	t_group("suspend/resume reject the wrong state");
	T_EQ("suspend.badstate", tu_setup_suspend(), TU_SETUP_ESTATE);
	T_EQ("resume.badstate", tu_setup_resume(), TU_SETUP_ESTATE);

	t_group("init builds + transmits M1 (CREATED -> M1_SENT)");
	T_EQ("init.ok", tu_setup_init(), TU_SETUP_OK);
	T_EQ("init.state", tu_setup_state(), TU_SETUP_M1_SENT);
	T_EQ("m1.proto", tu_rs_msg_proto(g_tx.msg), TU_RS_PROTO_RANGING);
	T_EQ("m1.id", tu_rs_msg_id(g_tx.msg), TU_RS_MSG_M1);
	T_EQ("m1.plen", tu_rs_msg_len(g_tx.msg), (uint16_t)(g_tx.len - 4u));
	T_EQ("m1.sid", tx_u32(TU_RS_ATTR_SESSION_IDENTIFIER, 0u), SID);
	T_EQ("m1.chan", tx_u8(TU_RS_ATTR_CHANNEL_BITMASK, 0xFFu), 0x01u);
	T_EQ("m1.len", g_tx.len, 20u);
	T_EQ("init.badstate", tu_setup_init(), TU_SETUP_ESTATE);

	t_group("handle guards + framing");
	T_EQ("mh.null", tu_setup_handle(NULL, 4u), TU_SETUP_EINVAL);
	T_EQ("mh.short", tu_setup_handle(g_tx.msg, 3u), TU_SETUP_EMALFORMED);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	b.buf[2] = 0x00u; /* claims payload 10, actual 0 */
	b.buf[3] = 0x0Au;
	T_EQ("mh.plen", tu_setup_handle(b.buf, b.len), TU_SETUP_EMALFORMED);
	b = mk(TU_RS_PROTO_AP, 0u);
	T_EQ("mh.unsupproto", feed(&b), TU_SETUP_EUNSUPPORTED);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_REQUEST);
	T_EQ("pr.unsup", feed(&b), TU_SETUP_EUNSUPPORTED);

	t_group("M2 advances to M3_SENT and transmits M3");
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_m2_attrs(&b);
	T_EQ("m2.ok", feed(&b), TU_SETUP_OK);
	T_EQ("m2.state", tu_setup_state(), TU_SETUP_M3_SENT);
	T_EQ("m3.id", tu_rs_msg_id(g_tx.msg), TU_RS_MSG_M3);
	/* The reader's minimum RAN multiplier (8) beats the peer's 4: 8 x 96 ms. */
	T_EQ("m3.ran", tx_u8(TU_RS_ATTR_RAN_MULTIPLIER, 0u), 8u);
	T_EQ("m3.chaps", tx_u8(TU_RS_ATTR_NUMBER_CHAPS_PER_SLOT, 0u), 3u);
	T_EQ("m3.responders", tx_u8(TU_RS_ATTR_NUMBER_RESPONDERS_NODES, 0u), 1u);
	T_EQ("m3.slots", tx_u8(TU_RS_ATTR_NUMBER_SLOTS_PER_ROUND, 0u), 12u);
	T_EQ("m3.sync", tx_u32(TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK, 0u), 0x05u);
	T_EQ("m3.hop", tx_u8(TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0u), 0x50u);
	T_EQ("m3.len", g_tx.len, 28u);
	T_EQ("m2.chan", tu_setup_params()->channel, 5);
	T_EQ("m2.slot", tu_setup_params()->slot_duration, 1200);

	t_group("M4 brings the radio up (M3_SENT -> RANGING)");
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M4);
	add_m4_attrs(&b);
	T_EQ("m4.ok", feed(&b), TU_SETUP_OK);
	T_EQ("m4.state", tu_setup_state(), TU_SETUP_RANGING);
	T_EQ("m4.active", g_ev.last, TU_SETUP_EV_ACTIVE);
	T_EQ("m4.listen", hw.listen_calls, 1);
	T_OK("m4.listening", tu_listening());
	T_EQ("m4.sid", tu_session_id(), SID);
	T_EQ("m4.sts0", tu_setup_params()->sts_index, 0x1000u);
	T_EQ("m4.code", tu_setup_params()->sync_code_index, 9);

	t_group("M2 vendor variant (mask + VENDOR_SPECIFIC attribute)");
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_m2_attrs(&b);
	add(&b, TU_RS_ATTR_VENDOR_SPECIFIC, (const uint8_t *)"\xa1\xb2\xc3", 3u);
	T_EQ("m2.vendor", feed(&b), TU_SETUP_OK);
	T_EQ("m2v.state", tu_setup_state(), TU_SETUP_M3_SENT);

	t_group("ranging error paths");
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u16(&b, TU_RS_ATTR_CONFIGURATION_IDENTIFIER, 1u);
	T_EQ("m2.missing", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_CHANNEL_BITMASK, 0x04u);
	T_EQ("m2.badchan", feed(&b), TU_SETUP_EINVAL);
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0x00u);
	T_EQ("m2.nohop", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_set_state(TU_SETUP_RANGING); /* wrong state for M2 */
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_m2_attrs(&b);
	T_EQ("m2.badstate", feed(&b), TU_SETUP_ESTATE);
	tu_setup_set_state(TU_SETUP_M1_SENT); /* wrong state for M4 */
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M4);
	add_m4_attrs(&b);
	T_EQ("m4.badstate", feed(&b), TU_SETUP_ESTATE);
	tu_setup_set_state(TU_SETUP_M3_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M4);
	add_u32(&b, TU_RS_ATTR_STS_INDEX0, 0x1000u);
	T_EQ("m4.missing", feed(&b), TU_SETUP_EMALFORMED);

	t_group("every attribute parser rejects a wrong-width value");
	{
		struct {
			uint8_t id;
			int width;
		} bad[] = {
			{TU_RS_ATTR_CONFIGURATION_IDENTIFIER, 1},
			{TU_RS_ATTR_PULSE_SHAPE_COMBO, 2},
			{TU_RS_ATTR_CHANNEL_BITMASK, 2},
			{TU_RS_ATTR_RAN_MULTIPLIER, 2},
			{TU_RS_ATTR_SLOT_BITMASK, 2},
			{TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK, 1},
			{TU_RS_ATTR_SYNC_CODE_INDEX, 2},
			{TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 2},
			{TU_RS_ATTR_STS_INDEX0, 1},
			{TU_RS_ATTR_UWB_TIME0, 1},
			{TU_RS_ATTR_HOP_MODE_KEY, 1},
			{TU_RS_ATTR_STATUS, 2},
			{TU_RS_ATTR_SESSION_IDENTIFIER, 1},
		};

		for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			tu_setup_set_state(TU_SETUP_M1_SENT);
			b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
			if (bad[i].width == 1) {
				add_u8(&b, bad[i].id, 0u);
			} else {
				add_u16(&b, bad[i].id, 0u);
			}
			T_EQ("attr.badwidth", feed(&b), TU_SETUP_EMALFORMED);
		}
	}

	t_group("unknown attribute id is ignored (parse continues)");
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_NUMBER_CHAPS_PER_SLOT, 0u);
	T_EQ("m2.unknownattr", feed(&b), TU_SETUP_EMALFORMED); /* then the mask check */

	t_group("suspend request handler (RANGING) stops the radio, transmits a response");
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_REQUEST);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID);
	T_EQ("susp.req", feed(&b), TU_SETUP_OK);
	T_EQ("susp.resp.id", tu_rs_msg_id(g_tx.msg), TU_RS_MSG_SUSPEND_RESPONSE);
	T_EQ("susp.resp.status", tx_u8(TU_RS_ATTR_STATUS, 0xFFu), TU_RS_STATUS_ACCEPT);
	T_EQ("susp.state", tu_setup_state(), TU_SETUP_SUSPENDED);
	T_OK("susp.radio_off", !tu_listening());

	t_group("suspend response reject keeps ranging");
	tu_setup_set_state(TU_SETUP_SUSPEND_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	add_u8(&b, TU_RS_ATTR_STATUS, TU_RS_STATUS_REJECT);
	T_EQ("susp.rej", feed(&b), TU_SETUP_OK);
	T_EQ("susp.rej.state", tu_setup_state(), TU_SETUP_RANGING);

	t_group("suspend response accept stops");
	tu_setup_set_state(TU_SETUP_SUSPEND_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	add_u8(&b, TU_RS_ATTR_STATUS, TU_RS_STATUS_ACCEPT);
	T_EQ("susp.acc", feed(&b), TU_SETUP_OK);
	T_EQ("susp.acc.state", tu_setup_state(), TU_SETUP_SUSPENDED);

	t_group("resume response restarts ranging (time offset applied)");
	tu_setup_set_time_offset(1000);
	tu_setup_set_state(TU_SETUP_RESUME_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_RESPONSE);
	add_u32(&b, TU_RS_ATTR_STS_INDEX0, 0x2000u);
	add_u64(&b, TU_RS_ATTR_UWB_TIME0, 0u);
	T_EQ("resume.resp", feed(&b), TU_SETUP_OK);
	T_EQ("resume.resp.state", tu_setup_state(), TU_SETUP_RANGING);
	T_EQ("resume.sts0", tu_setup_params()->sts_index, 0x2000u);
	T_EQ("resume.time", (long)tu_setup_params()->uwb_time_us, 1000L);
	T_OK("resume.listening", tu_listening());

	t_group("event notification (busy / general error / descriptor / unknown)");
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_EVENT);
	add(&b, TU_RS_EVENT_ATTR_BUSY, NULL, 0u);
	add_u8(&b, TU_RS_EVENT_ATTR_GENERAL_ERROR, 2u);
	add(&b, TU_RS_EVENT_ATTR_READER_DESCRIPTOR, NULL, 0u);
	add(&b, 0x7Fu, NULL, 0u);
	T_EQ("notif.event", feed(&b), TU_SETUP_OK);
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_EVENT);
	add_u16(&b, TU_RS_EVENT_ATTR_GENERAL_ERROR, 0u);
	T_EQ("notif.event.bad", feed(&b), TU_SETUP_EMALFORMED);

	t_group("ranging notification drives the state machine");
	{
		struct {
			uint8_t attr;
			enum tu_setup_state pre;
			enum tu_setup_state post;
		} rn[] = {
			{TU_RS_RANGING_ATTR_INIT_RANGING, TU_SETUP_CREATED, TU_SETUP_M1_SENT},
			{TU_RS_RANGING_ATTR_INIT_RANGING_SETUP_LATER, TU_SETUP_M1_SENT,
			 TU_SETUP_CREATED},
			{TU_RS_RANGING_ATTR_RANGING_SUSPENDED, TU_SETUP_RANGING, TU_SETUP_SUSPENDED},
			{TU_RS_RANGING_ATTR_INIT_RANGING_RESUME, TU_SETUP_SUSPENDED,
			 TU_SETUP_RESUME_REQ_SENT},
			{TU_RS_RANGING_ATTR_INIT_RANGING_RESUME_LATER, TU_SETUP_RESUME_REQ_SENT,
			 TU_SETUP_SUSPENDED},
		};

		for (size_t i = 0; i < sizeof(rn) / sizeof(rn[0]); i++) {
			tu_setup_set_state(rn[i].pre);
			b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
			add_u8(&b, rn[i].attr, 0u);
			T_EQ("rn.ok", feed(&b), TU_SETUP_OK);
			T_EQ("rn.state", tu_setup_state(), rn[i].post);
		}
	}
	T_EQ("resume.reqid", tu_rs_msg_id(g_tx.msg), TU_RS_MSG_RESUME_REQUEST);
	T_EQ("resume.reqsid", tx_u32(TU_RS_ATTR_SESSION_IDENTIFIER, 0u), SID);

	t_group("ranging notification: no-op attr + bad-state errors");
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
	add_u8(&b, TU_RS_RANGING_ATTR_SECURE_RANGING_FAILED, 0u);
	T_EQ("rn.secfail", feed(&b), TU_SETUP_OK);
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
	add_u8(&b, TU_RS_RANGING_ATTR_INIT_RANGING_SETUP_LATER, 0u);
	T_EQ("rn.badstate", feed(&b), TU_SETUP_ESTATE);
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
	add_u8(&b, TU_RS_RANGING_ATTR_INIT_RANGING_RESUME_LATER, 0u);
	T_EQ("rn.resumelater.badstate", feed(&b), TU_SETUP_ESTATE);
	tu_setup_set_state(TU_SETUP_CREATED);
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
	add_u8(&b, TU_RS_RANGING_ATTR_RANGING_SUSPENDED, 0u);
	T_EQ("rn.suspended.badstate", feed(&b), TU_SETUP_ESTATE);
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_RANGING);
	add_u8(&b, 0x7Fu, 0u);
	T_EQ("rn.unknownattr", feed(&b), TU_SETUP_OK);

	t_group("informational + unknown notification ids, supplementary service");
	b = mk(TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_READER_STATUS_CHANGED);
	T_EQ("notif.readerstatus", feed(&b), TU_SETUP_OK);
	b = mk(TU_RS_PROTO_NOTIFICATION, 0x7Fu);
	T_EQ("notif.unknownid", feed(&b), TU_SETUP_OK);
	b = mk(TU_RS_PROTO_SUPPLEMENTARY, 0x01u);
	add_u8(&b, 0x10u, 0xAAu);
	T_EQ("suppl.ok", feed(&b), TU_SETUP_OK);

	t_group("session-id mismatch rejects suspend request and M4");
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_REQUEST);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID ^ 1u);
	T_EQ("susp.badsid", feed(&b), TU_SETUP_EINVAL);
	tu_setup_set_state(TU_SETUP_M3_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M4);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID ^ 1u);
	T_EQ("m4.badsid", feed(&b), TU_SETUP_EINVAL);

	t_group("channel bitmask ch9 resolves to channel 9");
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_CHANNEL_BITMASK, 0x02u);
	T_EQ("m2.ch9", feed(&b), TU_SETUP_EMALFORMED); /* latched, then the mask check */
	T_EQ("m2.ch9.chan", tu_setup_params()->channel, 9);

	t_group("slot bitmask maps every chaps-per-slot bit");
	{
		struct {
			uint8_t bit;
			uint16_t slot_duration;
		} sb[] = {
			{0x02u, 400u * 4u},  {0x04u, 400u * 6u},  {0x08u, 400u * 8u},
			{0x10u, 400u * 9u},  {0x20u, 400u * 12u}, {0x40u, 400u * 24u},
			{0x80u, 400u * 0x80u}, /* no chaps bit: raw kept */
		};

		tu_setup_caps()->slot_bitmask = 0xFFu;
		for (size_t i = 0; i < sizeof(sb) / sizeof(sb[0]); i++) {
			tu_setup_set_state(TU_SETUP_M1_SENT);
			b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
			add_u8(&b, TU_RS_ATTR_SLOT_BITMASK, sb[i].bit);
			T_EQ("slot.parse", feed(&b), TU_SETUP_EMALFORMED);
			T_EQ("slot.duration", tu_setup_params()->slot_duration, sb[i].slot_duration);
		}
		tu_setup_caps()->slot_bitmask = 0x01u;
	}

	t_group("hopping preference selection: disabled / adaptive / unknown");
	tu_setup_caps()->hopping_config_bitmask = 0xFFu;
	tu_setup_caps()->hop_prefs[0] = TU_HOP_PREF_DISABLED;
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0xFFu);
	T_EQ("hop.disabled", feed(&b), TU_SETUP_EMALFORMED);
	T_EQ("hop.disabled.mode", tu_setup_params()->hopping_mode, TU_HOP_PREF_DISABLED);
	tu_setup_caps()->hop_prefs[0] = TU_HOP_PREF_ADAPTIVE_DEFAULT;
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0xFFu);
	T_EQ("hop.adaptive", feed(&b), TU_SETUP_EMALFORMED);
	T_EQ("hop.adaptive.mode", tu_setup_params()->hopping_mode, TU_HOP_PREF_ADAPTIVE_DEFAULT);
	tu_setup_caps()->hop_prefs[0] = (enum tu_hop_pref)0x55;
	tu_setup_set_state(TU_SETUP_M1_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M2);
	add_u8(&b, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, 0xFFu);
	T_EQ("hop.unknown", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_caps()->hop_prefs[0] = TU_HOP_PREF_CONTINUOUS_DEFAULT;
	tu_setup_caps()->hopping_config_bitmask = 0x0Au;

	t_group("suspend request: missing attributes + wrong state");
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_REQUEST);
	add_u8(&b, TU_RS_ATTR_STATUS, TU_RS_STATUS_ACCEPT);
	T_EQ("susp.req.missing", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_set_state(TU_SETUP_CREATED);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_REQUEST);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID);
	T_EQ("susp.req.badstate", feed(&b), TU_SETUP_ESTATE);

	t_group("suspend response: parse error, missing attrs, wrong state");
	tu_setup_set_state(TU_SETUP_SUSPEND_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID ^ 1u);
	T_EQ("susp.resp.badsid", feed(&b), TU_SETUP_EINVAL);
	tu_setup_set_state(TU_SETUP_SUSPEND_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	T_EQ("susp.resp.missing", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	add_u8(&b, TU_RS_ATTR_STATUS, TU_RS_STATUS_ACCEPT);
	T_EQ("susp.resp.badstate", feed(&b), TU_SETUP_ESTATE);

	t_group("resume response: parse error, missing attrs, wrong state");
	tu_setup_set_state(TU_SETUP_RESUME_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_RESPONSE);
	add_u32(&b, TU_RS_ATTR_SESSION_IDENTIFIER, SID ^ 1u);
	T_EQ("resume.badsid", feed(&b), TU_SETUP_EINVAL);
	tu_setup_set_state(TU_SETUP_RESUME_REQ_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_RESPONSE);
	add_u32(&b, TU_RS_ATTR_STS_INDEX0, 0x2000u);
	T_EQ("resume.missing", feed(&b), TU_SETUP_EMALFORMED);
	tu_setup_set_state(TU_SETUP_RANGING);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_RESPONSE);
	add_u32(&b, TU_RS_ATTR_STS_INDEX0, 0x2000u);
	add_u64(&b, TU_RS_ATTR_UWB_TIME0, 0u);
	T_EQ("resume.badstate", feed(&b), TU_SETUP_ESTATE);

	t_group("graceful suspend, forced suspend, resume request");
	tu_setup_set_state(TU_SETUP_RANGING);
	T_EQ("suspend.ok", tu_setup_suspend(), TU_SETUP_OK);
	T_EQ("suspend.state", tu_setup_state(), TU_SETUP_SUSPEND_REQ_SENT);
	T_EQ("suspend.reqid", tu_rs_msg_id(g_tx.msg), TU_RS_MSG_SUSPEND_REQUEST);
	tu_setup_set_state(TU_SETUP_RANGING);
	T_EQ("fsuspend.ok", tu_setup_forced_suspend(), TU_SETUP_OK);
	T_EQ("fsuspend.state", tu_setup_state(), TU_SETUP_SUSPENDED);
	T_EQ("resume.ok", tu_setup_resume(), TU_SETUP_OK);
	T_EQ("resume.state", tu_setup_state(), TU_SETUP_RESUME_REQ_SENT);

	t_group("a failed radio start answers the peer with a GeneralError");
	tu_setup_set_state(TU_SETUP_RESUME_REQ_SENT);
	hw.listen_ret = -5;
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_RESUME_RESPONSE);
	add_u32(&b, TU_RS_ATTR_STS_INDEX0, 0x2000u);
	add_u64(&b, TU_RS_ATTR_UWB_TIME0, 0u);
	T_EQ("resume.startfail", feed(&b), TU_SETUP_ESESSION_INIT);
	T_EQ("startfail.ev", g_ev.last, TU_SETUP_EV_ERROR);
	T_EQ("startfail.notify.proto", tu_rs_msg_proto(g_tx.msg), TU_RS_PROTO_NOTIFICATION);
	T_EQ("startfail.notify.id", tu_rs_msg_id(g_tx.msg), TU_RS_NOTIF_EVENT);
	T_EQ("startfail.notify.code", tx_u8(TU_RS_EVENT_ATTR_GENERAL_ERROR, 0xFFu), 0u);
	hw.listen_ret = 0;

	t_group("close stops the radio and forgets the session");
	tu_setup_set_state(TU_SETUP_M3_SENT);
	b = mk(TU_RS_PROTO_RANGING, TU_RS_MSG_M4);
	add_m4_attrs(&b);
	T_EQ("m4.again", feed(&b), TU_SETUP_OK);
	T_OK("m4.again.listening", tu_listening());
	tu_setup_close();
	T_OK("close.not_open", !tu_setup_is_open());
	T_OK("close.radio_off", !tu_listening());
	T_EQ("close.handle", tu_setup_handle(g_tx.msg, g_tx.len), TU_SETUP_EINVAL);
}
