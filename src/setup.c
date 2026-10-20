/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file setup.c — the UWB Ranging Service: M1-M4, suspend/resume, notifications. */

#include "tinyuwb/setup.h"

#include <string.h>

#include "tinyuwb/kdf.h"
#include "tinyuwb/uwb.h"

#ifndef TU_NUM_RESPONDERS
#define TU_NUM_RESPONDERS 1u
#endif
/* Slots per round the reader always offers in M3. */
#define TU_SLOTS_PER_ROUND_DEFAULT 12u
/* The one protocol version, 1.0. */
#define TU_RS_VERSION 0x0100u

/* Hopping-capability bits while selecting a hopping config (FiRa layout; CCC is << 3). */
#define HOP_CCC_TO_FIRA(v) ((uint8_t)((v) >> 3))
#define HOP_FIRA_TO_CCC(v) ((uint8_t)((v) << 3))
#define HOP_CAP_AES (1u << 0)
#define HOP_CAP_DEFAULT (1u << 1)
#define HOP_CAP_ADAPTIVE (1u << 2)
#define HOP_CAP_CONTINUOUS (1u << 3)
#define HOP_CAP_NO_HOPPING (1u << 4)
#define HOP_COMBO_CONTINUOUS_DEFAULT (HOP_CAP_CONTINUOUS | HOP_CAP_DEFAULT)
#define HOP_COMBO_ADAPTIVE_DEFAULT (HOP_CAP_ADAPTIVE | HOP_CAP_DEFAULT)
#define HOP_COMBO_NO_HOPPING (HOP_CAP_NO_HOPPING)

/* Attribute sets each message must carry, as a bit per id. */
#define M2_ATTR_MASK                                                                         \
	((1u << TU_RS_ATTR_CONFIGURATION_IDENTIFIER) | (1u << TU_RS_ATTR_PULSE_SHAPE_COMBO) |   \
	 (1u << TU_RS_ATTR_CHANNEL_BITMASK) | (1u << TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK) |      \
	 (1u << TU_RS_ATTR_RAN_MULTIPLIER) | (1u << TU_RS_ATTR_SLOT_BITMASK) |                  \
	 (1u << TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK))
#define M2_ATTR_MASK_VENDOR (M2_ATTR_MASK | (1u << TU_RS_ATTR_VENDOR_SPECIFIC))
#define M4_ATTR_MASK                                                                         \
	((1u << TU_RS_ATTR_STS_INDEX0) | (1u << TU_RS_ATTR_UWB_TIME0) |                         \
	 (1u << TU_RS_ATTR_HOP_MODE_KEY) | (1u << TU_RS_ATTR_SYNC_CODE_INDEX))
#define M4_ATTR_MASK_VENDOR (M4_ATTR_MASK | (1u << TU_RS_ATTR_VENDOR_SPECIFIC))
#define SUSPEND_REQUEST_ATTR_MASK (1u << TU_RS_ATTR_SESSION_IDENTIFIER)
#define SUSPEND_RESPONSE_ATTR_MASK (1u << TU_RS_ATTR_STATUS)
#define RESUME_RESPONSE_ATTR_MASK ((1u << TU_RS_ATTR_STS_INDEX0) | (1u << TU_RS_ATTR_UWB_TIME0))

/* The longest message built here is M3: 4 + 7 attributes. */
#define OUT_MAX 48u

static struct {
	bool open;
	struct tu_setup_caps caps;
	uint32_t session_id;
	uint8_t ursk[TU_URSK_LEN];
	tu_setup_tx_t tx;
	tu_setup_ev_t ev;
	void *user;
	int64_t time_offset;
	enum tu_setup_state state;
	bool radio_up;
	struct tu_setup_params p;
} s;

/* ── Framing ────────────────────────────────────────────────────────────── */

struct out {
	uint8_t buf[OUT_MAX];
	size_t len;
	bool ok;
};

static void out_header(struct out *o, uint8_t proto, uint8_t id)
{
	o->buf[0] = proto;
	o->buf[1] = id;
	o->buf[2] = 0u;
	o->buf[3] = 0u;
	o->len = TU_RS_HEADER_LEN;
	o->ok = true;
}

static void out_attr(struct out *o, uint8_t id, const uint8_t *v, uint8_t n)
{
	if (o->len + TU_RS_ATTR_HEADER_LEN + n > sizeof(o->buf)) {
		o->ok = false;
		return;
	}
	o->buf[o->len++] = id;
	o->buf[o->len++] = n;
	memcpy(&o->buf[o->len], v, n);
	o->len += n;
}

static void out_u8(struct out *o, uint8_t id, uint8_t v)
{
	out_attr(o, id, &v, 1u);
}

static void out_u16(struct out *o, uint8_t id, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};

	out_attr(o, id, b, sizeof(b));
}

static void out_u32(struct out *o, uint8_t id, uint32_t v)
{
	uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};

	out_attr(o, id, b, sizeof(b));
}

/** Fix the payload length and hand the message to the caller. */
static enum tu_setup_err send(struct out *o)
{
	uint16_t plen = (uint16_t)(o->len - TU_RS_HEADER_LEN);

	if (!o->ok) {
		return TU_SETUP_EINTERNAL;
	}
	o->buf[2] = (uint8_t)(plen >> 8);
	o->buf[3] = (uint8_t)plen;
	if (s.tx != NULL) {
		s.tx(o->buf, o->len, s.user);
	}
	return TU_SETUP_OK;
}

static void emit(enum tu_setup_event e)
{
	if (s.ev != NULL) {
		s.ev(e, s.user);
	}
}

struct attr {
	uint8_t id;
	uint8_t len;
	const uint8_t *val;
};

/** Walk the attributes of a framed message; false at the end or on an overrun. */
static bool next_attr(const uint8_t *msg, size_t len, size_t *off, struct attr *a)
{
	if (*off + TU_RS_ATTR_HEADER_LEN > len) {
		return false;
	}
	a->id = msg[*off];
	a->len = msg[*off + 1u];
	a->val = &msg[*off + 2u];
	*off += TU_RS_ATTR_HEADER_LEN + a->len;
	if (*off > len) {
		*off = len;
		return false;
	}
	return true;
}

static bool read_be(const struct attr *a, uint8_t width, uint64_t *out)
{
	uint64_t v = 0u;

	if (a->len != width) {
		return false;
	}
	for (uint8_t i = 0u; i < width; i++) {
		v = (v << 8) | a->val[i];
	}
	*out = v;
	return true;
}

/* ── Builders ───────────────────────────────────────────────────────────── */

static enum tu_setup_err send_m1(void)
{
	struct out o;

	out_header(&o, TU_RS_PROTO_RANGING, TU_RS_MSG_M1);
	out_u16(&o, TU_RS_ATTR_CONFIGURATION_IDENTIFIER, s.caps.uwb_config_id);
	out_u8(&o, TU_RS_ATTR_PULSE_SHAPE_COMBO, s.caps.pulse_shape_combo);
	out_u8(&o, TU_RS_ATTR_CHANNEL_BITMASK, s.caps.channel_bitmask);
	out_u32(&o, TU_RS_ATTR_SESSION_IDENTIFIER, s.session_id);
	return send(&o);
}

static enum tu_setup_err send_m3(void)
{
	struct out o;

	/* Derive the count fields from the M2 durations and commit the reader's choices. */
	s.p.slot_per_rr = TU_SLOTS_PER_ROUND_DEFAULT;
	s.p.mac_mode = s.caps.mac_mode;
	out_header(&o, TU_RS_PROTO_RANGING, TU_RS_MSG_M3);
	out_u8(&o, TU_RS_ATTR_RAN_MULTIPLIER, (uint8_t)(s.p.ranging_duration_ms / 96u));
	out_u8(&o, TU_RS_ATTR_NUMBER_CHAPS_PER_SLOT, (uint8_t)(s.p.slot_duration / 400u));
	/* Must equal rcfg[12]: both feed the SaltedHash the phone recomputes. */
	out_u8(&o, TU_RS_ATTR_NUMBER_RESPONDERS_NODES, (uint8_t)TU_NUM_RESPONDERS);
	out_u8(&o, TU_RS_ATTR_NUMBER_SLOTS_PER_ROUND, s.p.slot_per_rr);
	out_u32(&o, TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK, s.caps.sync_code_index_bitmask);
	out_u8(&o, TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK, s.p.hopping_config_bitmask);
	out_u8(&o, TU_RS_ATTR_MAC_MODE, s.caps.mac_mode);
	return send(&o);
}

static enum tu_setup_err send_request(bool suspend)
{
	struct out o;

	out_header(&o, TU_RS_PROTO_RANGING,
		   suspend ? TU_RS_MSG_SUSPEND_REQUEST : TU_RS_MSG_RESUME_REQUEST);
	out_u32(&o, TU_RS_ATTR_SESSION_IDENTIFIER, s.session_id);
	return send(&o);
}

static enum tu_setup_err send_suspend_response(uint8_t status)
{
	struct out o;

	out_header(&o, TU_RS_PROTO_RANGING, TU_RS_MSG_SUSPEND_RESPONSE);
	out_u8(&o, TU_RS_ATTR_STATUS, status);
	return send(&o);
}

static enum tu_setup_err send_general_error(uint8_t code)
{
	struct out o;

	out_header(&o, TU_RS_PROTO_NOTIFICATION, TU_RS_NOTIF_EVENT);
	out_u8(&o, TU_RS_EVENT_ATTR_GENERAL_ERROR, code);
	return send(&o);
}

/* ── Radio start/stop ───────────────────────────────────────────────────── */

/** Start the responder with the negotiated parameters; builds the RangingConfiguration
 * (the SaltedHash input the phone hashes). */
static enum tu_setup_err radio_start(void)
{
	struct tu_cfg c;
	uint8_t rcfg[17];

	memset(&c, 0, sizeof(c));
	c.session_id = s.p.session_id;
	c.channel = s.p.channel;
	c.sync_code_index = s.p.sync_code_index;
	c.slot_duration_rstu = s.p.slot_duration;
	c.block_duration_ms = s.p.ranging_duration_ms;
	c.slot_per_round = s.p.slot_per_rr;
	c.sts_index0 = s.p.sts_index;
	c.uwb_time_us = s.p.uwb_time_us;
	c.ursk = s.ursk;
	/* BE Protocol_Version, UWB_Config_Id, UWB_Session_Id, STS_Index0, then five bytes. */
	rcfg[0] = 0x01u;
	rcfg[1] = 0x00u;
	rcfg[2] = (uint8_t)(s.p.uwb_config_id >> 8);
	rcfg[3] = (uint8_t)s.p.uwb_config_id;
	rcfg[4] = (uint8_t)(s.p.session_id >> 24);
	rcfg[5] = (uint8_t)(s.p.session_id >> 16);
	rcfg[6] = (uint8_t)(s.p.session_id >> 8);
	rcfg[7] = (uint8_t)s.p.session_id;
	rcfg[8] = (uint8_t)(s.p.sts_index >> 24);
	rcfg[9] = (uint8_t)(s.p.sts_index >> 16);
	rcfg[10] = (uint8_t)(s.p.sts_index >> 8);
	rcfg[11] = (uint8_t)s.p.sts_index;
	rcfg[12] = (uint8_t)TU_NUM_RESPONDERS;
	rcfg[13] = (uint8_t)(s.p.ranging_duration_ms / 96u);
	rcfg[14] = s.p.slot_per_rr;
	rcfg[15] = (uint8_t)(s.p.slot_duration / 400u);
	rcfg[16] = s.p.pulse_shape_combo;
	c.ranging_config = rcfg;
	c.rc_len = sizeof(rcfg);

	if (tu_start(&c) != 0) {
		(void)send_general_error(TU_RS_GENERAL_ERROR_UNKNOWN);
		emit(TU_SETUP_EV_ERROR);
		return TU_SETUP_ESESSION_INIT;
	}
	s.radio_up = true;
	emit(TU_SETUP_EV_IDLE);
	emit(TU_SETUP_EV_ACTIVE);
	return TU_SETUP_OK;
}

static enum tu_setup_err radio_stop(void)
{
	if (s.radio_up) {
		tu_stop();
		s.radio_up = false;
	}
	emit(TU_SETUP_EV_IDLE);
	s.state = TU_SETUP_SUSPENDED;
	return TU_SETUP_OK;
}

/** Without a synchronized time base, start as soon as possible. */
static void compute_initiation_time(void)
{
	if (s.time_offset == 0) {
		s.p.uwb_time_us = 0u;
	} else {
		s.p.uwb_time_us += (uint64_t)s.time_offset;
	}
}

/* ── Attribute parsers (fold values into the negotiated parameters) ─────── */

static enum tu_setup_err parse_slot_bitmask(uint8_t bitmask)
{
	/* Smallest NChap-per-slot supported by both sides = lowest common bit. */
	uint8_t common = bitmask & s.caps.slot_bitmask;
	uint8_t chaps = common;
	uint8_t idx;
	static const uint8_t chap_of_bit[7] = {3u, 4u, 6u, 8u, 9u, 12u, 24u};

	for (idx = 0u; idx < 7u; idx++) {
		if ((common >> idx) & 1u) {
			chaps = chap_of_bit[idx];
			break;
		}
	}
	s.p.slot_duration = (uint16_t)(400u * chaps);
	return TU_SETUP_OK;
}

static enum tu_setup_err parse_hopping(uint8_t peer)
{
	uint8_t common = HOP_CCC_TO_FIRA(peer) & s.caps.hopping_config_bitmask;

	/* The first preferred config both sides fully support. */
	for (uint8_t i = 0u; i < s.caps.hop_pref_count; i++) {
		uint8_t combo = 0u;

		switch (s.caps.hop_prefs[i]) {
		case TU_HOP_PREF_DISABLED:
			combo = HOP_COMBO_NO_HOPPING;
			break;
		case TU_HOP_PREF_CONTINUOUS_DEFAULT:
			combo = HOP_COMBO_CONTINUOUS_DEFAULT;
			break;
		case TU_HOP_PREF_ADAPTIVE_DEFAULT:
			combo = HOP_COMBO_ADAPTIVE_DEFAULT;
			break;
		default:
			break;
		}
		if (combo != 0u && (common & combo) == combo) {
			s.p.hopping_mode = s.caps.hop_prefs[i];
			s.p.hopping_config_bitmask = HOP_FIRA_TO_CCC(combo);
			return TU_SETUP_OK;
		}
	}
	return TU_SETUP_EMALFORMED;
}

static enum tu_setup_err parse_attr(const struct attr *a, uint8_t *status)
{
	uint64_t v;

	switch (a->id) {
	case TU_RS_ATTR_CONFIGURATION_IDENTIFIER:
		if (!read_be(a, 2u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		s.p.uwb_config_id = (uint16_t)v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_PULSE_SHAPE_COMBO:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		s.p.pulse_shape_combo = (uint8_t)v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_SESSION_IDENTIFIER:
		if (!read_be(a, 4u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		return (uint32_t)v == s.session_id ? TU_SETUP_OK : TU_SETUP_EINVAL;
	case TU_RS_ATTR_CHANNEL_BITMASK:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		if (v == TU_RS_CHANNEL_BITMASK_CH5) {
			s.p.channel = 5u;
		} else if (v == TU_RS_CHANNEL_BITMASK_CH9) {
			s.p.channel = 9u;
		} else {
			return TU_SETUP_EINVAL;
		}
		return TU_SETUP_OK;
	case TU_RS_ATTR_RAN_MULTIPLIER:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		/* The larger of the peer's value and the reader's minimum. */
		if (v < s.caps.min_ran_multiplier) {
			v = s.caps.min_ran_multiplier;
		}
		s.p.ranging_duration_ms = 96u * (uint32_t)v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_SLOT_BITMASK:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		return parse_slot_bitmask((uint8_t)v);
	case TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK:
		/* The peer's offer; the reader keeps its own bitmask for M3. */
		return read_be(a, 4u, &v) ? TU_SETUP_OK : TU_SETUP_EMALFORMED;
	case TU_RS_ATTR_SYNC_CODE_INDEX:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		s.p.sync_code_index = (uint8_t)v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		return parse_hopping((uint8_t)v);
	case TU_RS_ATTR_STS_INDEX0:
		if (!read_be(a, 4u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		s.p.sts_index = (uint32_t)v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_UWB_TIME0:
		if (!read_be(a, 8u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		s.p.uwb_time_us = v;
		return TU_SETUP_OK;
	case TU_RS_ATTR_HOP_MODE_KEY:
		if (!read_be(a, 4u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		memcpy(s.p.hop_mode_key, a->val, 4u);
		return TU_SETUP_OK;
	case TU_RS_ATTR_STATUS:
		if (!read_be(a, 1u, &v)) {
			return TU_SETUP_EMALFORMED;
		}
		*status = (uint8_t)v;
		return TU_SETUP_OK;
	default:
		/* Vendor-specific and unknown ids: present, ignored. */
		return TU_SETUP_OK;
	}
}

static enum tu_setup_err parse_ranging(const uint8_t *msg, size_t len, uint32_t *mask,
				       uint8_t *status)
{
	size_t off = TU_RS_HEADER_LEN;
	struct attr a;

	*mask = 0u;
	*status = 0xFFu;
	while (next_attr(msg, len, &off, &a)) {
		enum tu_setup_err e;

		if (a.id < TU_RS_ATTR_MAX) {
			*mask |= 1u << a.id;
		}
		e = parse_attr(&a, status);
		if (e != TU_SETUP_OK) {
			return e;
		}
	}
	return TU_SETUP_OK;
}

/* ── Ranging Service handlers ───────────────────────────────────────────── */

static enum tu_setup_err handle_m2(const uint8_t *msg, size_t len)
{
	uint32_t mask;
	uint8_t status;
	enum tu_setup_err e = parse_ranging(msg, len, &mask, &status);

	if (e != TU_SETUP_OK) {
		return e;
	}
	if (mask != M2_ATTR_MASK && mask != M2_ATTR_MASK_VENDOR) {
		return TU_SETUP_EMALFORMED;
	}
	if (s.state != TU_SETUP_M1_SENT) {
		return TU_SETUP_ESTATE;
	}
	e = send_m3();
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_M3_SENT;
	return TU_SETUP_OK;
}

static enum tu_setup_err handle_m4(const uint8_t *msg, size_t len)
{
	uint32_t mask;
	uint8_t status;
	enum tu_setup_err e = parse_ranging(msg, len, &mask, &status);

	if (e != TU_SETUP_OK) {
		return e;
	}
	if (mask != M4_ATTR_MASK && mask != M4_ATTR_MASK_VENDOR) {
		return TU_SETUP_EMALFORMED;
	}
	if (s.state != TU_SETUP_M3_SENT) {
		return TU_SETUP_ESTATE;
	}
	compute_initiation_time();
	e = radio_start();
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_RANGING;
	return TU_SETUP_OK;
}

static enum tu_setup_err handle_suspend_request(const uint8_t *msg, size_t len)
{
	uint32_t mask;
	uint8_t status;
	enum tu_setup_err e = parse_ranging(msg, len, &mask, &status);

	if (e != TU_SETUP_OK) {
		return e;
	}
	if (mask != SUSPEND_REQUEST_ATTR_MASK) {
		return TU_SETUP_EMALFORMED;
	}
	if (s.state != TU_SETUP_RANGING) {
		return TU_SETUP_ESTATE;
	}
	e = radio_stop();
	return send_suspend_response(e == TU_SETUP_OK ? TU_RS_STATUS_ACCEPT
						       : TU_RS_STATUS_REJECT);
}

static enum tu_setup_err handle_suspend_response(const uint8_t *msg, size_t len)
{
	uint32_t mask;
	uint8_t status;
	enum tu_setup_err e = parse_ranging(msg, len, &mask, &status);

	if (e != TU_SETUP_OK) {
		return e;
	}
	if (mask != SUSPEND_RESPONSE_ATTR_MASK) {
		return TU_SETUP_EMALFORMED;
	}
	if (s.state != TU_SETUP_SUSPEND_REQ_SENT) {
		return TU_SETUP_ESTATE;
	}
	if (status != TU_RS_STATUS_ACCEPT) {
		s.state = TU_SETUP_RANGING;
		return TU_SETUP_OK;
	}
	return radio_stop();
}

static enum tu_setup_err handle_resume_response(const uint8_t *msg, size_t len)
{
	uint32_t mask;
	uint8_t status;
	enum tu_setup_err e = parse_ranging(msg, len, &mask, &status);

	if (e != TU_SETUP_OK) {
		return e;
	}
	if (mask != RESUME_RESPONSE_ATTR_MASK) {
		return TU_SETUP_EMALFORMED;
	}
	if (s.state != TU_SETUP_RESUME_REQ_SENT) {
		return TU_SETUP_ESTATE;
	}
	compute_initiation_time();
	e = radio_start();
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_RANGING;
	return TU_SETUP_OK;
}

static enum tu_setup_err process_ranging(const uint8_t *msg, size_t len)
{
	switch (tu_rs_msg_id(msg)) {
	case TU_RS_MSG_M2:
		return handle_m2(msg, len);
	case TU_RS_MSG_M4:
		return handle_m4(msg, len);
	case TU_RS_MSG_SUSPEND_REQUEST:
		return handle_suspend_request(msg, len);
	case TU_RS_MSG_SUSPEND_RESPONSE:
		return handle_suspend_response(msg, len);
	case TU_RS_MSG_RESUME_RESPONSE:
		return handle_resume_response(msg, len);
	default:
		return TU_SETUP_EUNSUPPORTED;
	}
}

/* ── Notification handlers ──────────────────────────────────────────────── */

static enum tu_setup_err process_event(const uint8_t *msg, size_t len)
{
	size_t off = TU_RS_HEADER_LEN;
	struct attr a;

	while (next_attr(msg, len, &off, &a)) {
		if (a.id == TU_RS_EVENT_ATTR_GENERAL_ERROR && a.len != 1u) {
			return TU_SETUP_EMALFORMED;
		}
		/* busy, reader descriptor, unknown: informational */
	}
	return TU_SETUP_OK;
}

static enum tu_setup_err process_ranging_notification(const uint8_t *msg, size_t len)
{
	size_t off = TU_RS_HEADER_LEN;
	struct attr a;
	enum tu_setup_err e = TU_SETUP_OK;

	while (next_attr(msg, len, &off, &a)) {
		switch (a.id) {
		case TU_RS_RANGING_ATTR_INIT_RANGING:
			e = tu_setup_init();
			break;
		case TU_RS_RANGING_ATTR_INIT_RANGING_RESUME:
			e = tu_setup_resume();
			break;
		case TU_RS_RANGING_ATTR_INIT_RANGING_SETUP_LATER:
			if (s.state != TU_SETUP_M1_SENT) {
				return TU_SETUP_ESTATE;
			}
			s.state = TU_SETUP_CREATED;
			break;
		case TU_RS_RANGING_ATTR_INIT_RANGING_RESUME_LATER:
			if (s.state != TU_SETUP_RESUME_REQ_SENT) {
				return TU_SETUP_ESTATE;
			}
			s.state = TU_SETUP_SUSPENDED;
			break;
		case TU_RS_RANGING_ATTR_RANGING_SUSPENDED:
			if (s.state != TU_SETUP_RANGING) {
				return TU_SETUP_ESTATE;
			}
			e = radio_stop();
			break;
		default: /* secure-ranging-failed and unknown: nothing to drive */
			break;
		}
		if (e != TU_SETUP_OK) {
			return e;
		}
	}
	return TU_SETUP_OK;
}

static enum tu_setup_err process_notification(const uint8_t *msg, size_t len)
{
	switch (tu_rs_msg_id(msg)) {
	case TU_RS_NOTIF_EVENT:
		return process_event(msg, len);
	case TU_RS_NOTIF_RANGING:
		return process_ranging_notification(msg, len);
	default: /* reader status and unknown ids: informational */
		return TU_SETUP_OK;
	}
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int tu_setup_open(const struct tu_setup_caps *caps, uint32_t session_id, const uint8_t ursk[32],
		  tu_setup_tx_t tx, tu_setup_ev_t ev, void *user)
{
	if (caps == NULL || ursk == NULL || tx == NULL) {
		return -1;
	}
	tu_setup_close();
	s.caps = *caps;
	s.session_id = session_id;
	memcpy(s.ursk, ursk, sizeof(s.ursk));
	s.tx = tx;
	s.ev = ev;
	s.user = user;
	s.p.session_id = session_id;
	s.state = TU_SETUP_CREATED;
	s.open = true;
	return 0;
}

void tu_setup_close(void)
{
	if (s.radio_up) {
		tu_stop();
	}
	memset(&s, 0, sizeof(s));
}

bool tu_setup_is_open(void)
{
	return s.open;
}

enum tu_setup_err tu_setup_handle(const uint8_t *msg, size_t len)
{
	if (!s.open || msg == NULL) {
		return TU_SETUP_EINVAL;
	}
	if (len < TU_RS_HEADER_LEN || len > TU_RS_MSG_MAX ||
	    tu_rs_msg_len(msg) != len - TU_RS_HEADER_LEN) {
		return TU_SETUP_EMALFORMED;
	}
	switch (tu_rs_msg_proto(msg)) {
	case TU_RS_PROTO_RANGING:
		return process_ranging(msg, len);
	case TU_RS_PROTO_NOTIFICATION:
		return process_notification(msg, len);
	case TU_RS_PROTO_SUPPLEMENTARY:
		return TU_SETUP_OK; /* the phone echoes defaults; nothing modelled */
	default:
		return TU_SETUP_EUNSUPPORTED;
	}
}

enum tu_setup_err tu_setup_init(void)
{
	enum tu_setup_err e;

	if (!s.open) {
		return TU_SETUP_EINVAL;
	}
	if (s.state != TU_SETUP_CREATED) {
		return TU_SETUP_ESTATE;
	}
	e = send_m1();
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_M1_SENT;
	return TU_SETUP_OK;
}

enum tu_setup_err tu_setup_suspend(void)
{
	enum tu_setup_err e;

	if (!s.open) {
		return TU_SETUP_EINVAL;
	}
	if (s.state != TU_SETUP_RANGING) {
		return TU_SETUP_ESTATE;
	}
	e = send_request(true);
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_SUSPEND_REQ_SENT;
	return TU_SETUP_OK;
}

enum tu_setup_err tu_setup_forced_suspend(void)
{
	if (!s.open) {
		return TU_SETUP_EINVAL;
	}
	return radio_stop();
}

enum tu_setup_err tu_setup_resume(void)
{
	enum tu_setup_err e;

	if (!s.open) {
		return TU_SETUP_EINVAL;
	}
	if (s.state != TU_SETUP_SUSPENDED) {
		return TU_SETUP_ESTATE;
	}
	e = send_request(false);
	if (e != TU_SETUP_OK) {
		return e;
	}
	s.state = TU_SETUP_RESUME_REQ_SENT;
	return TU_SETUP_OK;
}

void tu_setup_set_time_offset(int64_t offset_us)
{
	s.time_offset = offset_us;
}

enum tu_setup_state tu_setup_state(void)
{
	return s.state;
}

void tu_setup_set_state(enum tu_setup_state st)
{
	s.state = st;
}

const struct tu_setup_params *tu_setup_params(void)
{
	return &s.p;
}

struct tu_setup_caps *tu_setup_caps(void)
{
	return &s.caps;
}
