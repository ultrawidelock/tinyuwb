/** @file setup.h — the UWB Ranging Service over BLE: M1-M4 session setup, suspend/resume
 * and notifications, driving the responder engine (tinyuwb/uwb.h).
 *
 * One session at a time (the radio is single-session). Messages in and out are the
 * plaintext [proto][id][len_be16][attributes]; the caller seals and sends them on its
 * BLE channel and hands received ones in through tu_setup_handle().
 */

#ifndef TINYUWB_SETUP_H
#define TINYUWB_SETUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Framing. */
#define TU_RS_HEADER_LEN 4u /**< proto + id + 2-byte big-endian payload length */
#define TU_RS_ATTR_HEADER_LEN 2u /**< attribute id + length */
#define TU_RS_MSG_MAX 256u /**< largest inbound SDU */

/* Protocol type (byte 0). */
#define TU_RS_PROTO_AP 0x00u
#define TU_RS_PROTO_RANGING 0x01u
#define TU_RS_PROTO_NOTIFICATION 0x02u
#define TU_RS_PROTO_SUPPLEMENTARY 0x03u

/* Ranging Service message ids (byte 1). */
#define TU_RS_MSG_M1 0x00u
#define TU_RS_MSG_M2 0x01u
#define TU_RS_MSG_M3 0x02u
#define TU_RS_MSG_M4 0x03u
#define TU_RS_MSG_SUSPEND_REQUEST 0x04u
#define TU_RS_MSG_SUSPEND_RESPONSE 0x05u
#define TU_RS_MSG_RESUME_REQUEST 0x06u
#define TU_RS_MSG_RESUME_RESPONSE 0x07u

/* Ranging Service attribute ids. */
#define TU_RS_ATTR_CONFIGURATION_IDENTIFIER 0x00u
#define TU_RS_ATTR_PULSE_SHAPE_COMBO 0x01u
#define TU_RS_ATTR_SESSION_IDENTIFIER 0x02u
#define TU_RS_ATTR_CHANNEL_BITMASK 0x03u
#define TU_RS_ATTR_RAN_MULTIPLIER 0x04u
#define TU_RS_ATTR_SLOT_BITMASK 0x05u
#define TU_RS_ATTR_SYNC_CODE_INDEX_BITMASK 0x06u
#define TU_RS_ATTR_SYNC_CODE_INDEX 0x07u
#define TU_RS_ATTR_HOPPING_CONFIGURATION_BITMASK 0x08u
#define TU_RS_ATTR_NUMBER_CHAPS_PER_SLOT 0x09u
#define TU_RS_ATTR_NUMBER_RESPONDERS_NODES 0x0Au
#define TU_RS_ATTR_NUMBER_SLOTS_PER_ROUND 0x0Bu
#define TU_RS_ATTR_STS_INDEX0 0x0Cu
#define TU_RS_ATTR_UWB_TIME0 0x0Du
#define TU_RS_ATTR_HOP_MODE_KEY 0x0Eu
#define TU_RS_ATTR_MAC_MODE 0x0Fu
#define TU_RS_ATTR_VENDOR_SPECIFIC 0x10u
#define TU_RS_ATTR_STATUS 0x11u
#define TU_RS_ATTR_MAX 0x12u

#define TU_RS_CHANNEL_BITMASK_CH5 0x01u
#define TU_RS_CHANNEL_BITMASK_CH9 0x02u

#define TU_RS_STATUS_ACCEPT 0u
#define TU_RS_STATUS_REJECT 1u

/* Notification message ids. */
#define TU_RS_NOTIF_EVENT 0x00u
#define TU_RS_NOTIF_RANGING 0x01u
#define TU_RS_NOTIF_READER_STATUS_CHANGED 0x02u
#define TU_RS_NOTIF_READER_STATUS_AP 0x03u
#define TU_RS_NOTIF_PKE_REQUEST 0x04u
#define TU_RS_NOTIF_INITIATE_AP 0x05u
#define TU_RS_NOTIF_INITIATE_AP_RKE 0x06u

/* Event notification attributes. */
#define TU_RS_EVENT_ATTR_BUSY 0x00u
#define TU_RS_EVENT_ATTR_GENERAL_ERROR 0x01u
#define TU_RS_EVENT_ATTR_READER_DESCRIPTOR 0x02u

#define TU_RS_GENERAL_ERROR_UNKNOWN 0u
#define TU_RS_GENERAL_ERROR_RESOURCE_UNAVAILABLE 1u
#define TU_RS_GENERAL_ERROR_WRONG_PARAMS 2u
#define TU_RS_GENERAL_ERROR_URSK_UNAVAILABLE 3u

/* Ranging notification attributes. */
#define TU_RS_RANGING_ATTR_INIT_RANGING 0x00u
#define TU_RS_RANGING_ATTR_INIT_RANGING_RESUME 0x01u
#define TU_RS_RANGING_ATTR_INIT_RANGING_SETUP_LATER 0x02u
#define TU_RS_RANGING_ATTR_INIT_RANGING_RESUME_LATER 0x03u
#define TU_RS_RANGING_ATTR_SECURE_RANGING_FAILED 0x04u
#define TU_RS_RANGING_ATTR_RANGING_SUSPENDED 0x05u

enum tu_setup_err {
	TU_SETUP_OK = 0,
	TU_SETUP_EINVAL,       /**< bad argument or session-id mismatch */
	TU_SETUP_EINTERNAL,    /**< a message could not be built */
	TU_SETUP_ESESSION_INIT,/**< the radio could not start */
	TU_SETUP_ESESSION_CONFIG, /**< no URSK */
	TU_SETUP_EUNSUPPORTED, /**< unknown protocol or message id */
	TU_SETUP_ESTATE,       /**< message or call out of sequence */
	TU_SETUP_EMALFORMED,   /**< framing or attribute error */
};

enum tu_setup_state {
	TU_SETUP_CREATED,
	TU_SETUP_M1_SENT,
	TU_SETUP_M3_SENT,
	TU_SETUP_RANGING,
	TU_SETUP_SUSPEND_REQ_SENT,
	TU_SETUP_SUSPENDED,
	TU_SETUP_RESUME_REQ_SENT,
};

/** Hopping configurations the reader may prefer, in the CCC encoding. */
enum tu_hop_pref {
	TU_HOP_PREF_DISABLED = 0x00,
	TU_HOP_PREF_CONTINUOUS_DEFAULT = 0xA1,
	TU_HOP_PREF_ADAPTIVE_DEFAULT = 0xA3,
};

/** The reader's advertised capabilities and selection preferences. */
struct tu_setup_caps {
	uint8_t slot_bitmask;             /**< supported slot durations, one bit per NChap value */
	uint32_t sync_code_index_bitmask; /**< usable SYNC code indices, bit i => code (i+1) */
	uint8_t hopping_config_bitmask;   /**< supported hopping modes (FiRa bit layout) */
	uint8_t channel_bitmask;          /**< b0 => ch5, b1 => ch9 */
	uint16_t uwb_config_id;           /**< the one UWB configuration offered in M1 */
	uint8_t pulse_shape_combo;        /**< the one pulse-shape combo offered in M1 */
	uint8_t min_ran_multiplier;       /**< lower bound on the RAN multiplier (block = N x 96 ms) */
	enum tu_hop_pref hop_prefs[3];    /**< ordered hopping preferences */
	uint8_t hop_pref_count;
	uint8_t mac_mode;                 /**< b0-b5 round offset, b6-b7 number of rounds */
};

/** The parameters negotiated across M1-M4, as the engine receives them. */
struct tu_setup_params {
	uint32_t session_id;
	uint16_t uwb_config_id;
	uint8_t pulse_shape_combo;
	uint8_t channel;
	uint8_t sync_code_index;
	uint32_t ranging_duration_ms;
	uint8_t slot_per_rr;
	uint16_t slot_duration;
	enum tu_hop_pref hopping_mode;
	uint8_t hopping_config_bitmask;
	uint8_t hop_mode_key[4];
	uint32_t sts_index;
	uint8_t mac_mode;
	uint64_t uwb_time_us;
};

enum tu_setup_event {
	TU_SETUP_EV_ACTIVE, /**< the radio is ranging with the negotiated parameters */
	TU_SETUP_EV_IDLE,   /**< ranging stopped */
	TU_SETUP_EV_ERROR,  /**< the radio could not start; a GeneralError went to the peer */
};

/** Outbound plaintext SDU to seal and send on the BLE channel. */
typedef void (*tu_setup_tx_t)(const uint8_t *msg, size_t len, void *user);
typedef void (*tu_setup_ev_t)(enum tu_setup_event ev, void *user);

/** Open the session: remembers the caps, id and URSK; sends nothing until the peer's
 * Initiate-Ranging-Session arrives. Replaces any session already open. */
int tu_setup_open(const struct tu_setup_caps *caps, uint32_t session_id, const uint8_t ursk[32],
		  tu_setup_tx_t tx, tu_setup_ev_t ev, void *user);

/** Stop ranging if up and forget the session. */
void tu_setup_close(void);

bool tu_setup_is_open(void);

/** Handle an inbound SDU (M2, M4, suspend/resume, notifications, supplementary). */
enum tu_setup_err tu_setup_handle(const uint8_t *msg, size_t len);

/** Begin setup: build and send M1 (CREATED -> M1_SENT). */
enum tu_setup_err tu_setup_init(void);

/** Graceful suspend: send a suspend request (RANGING -> SUSPEND_REQ_SENT). */
enum tu_setup_err tu_setup_suspend(void);

/** Stop ranging now without a request (-> SUSPENDED). */
enum tu_setup_err tu_setup_forced_suspend(void);

/** Send a resume request (SUSPENDED -> RESUME_REQ_SENT). */
enum tu_setup_err tu_setup_resume(void);

/** Microseconds added to the negotiated UWB_Time0; 0 = start as soon as possible. */
void tu_setup_set_time_offset(int64_t offset_us);

enum tu_setup_state tu_setup_state(void);
void tu_setup_set_state(enum tu_setup_state s); /* tests */
const struct tu_setup_params *tu_setup_params(void);
struct tu_setup_caps *tu_setup_caps(void); /* tests: the live copy */

/* Header accessors for a framed message. */
static inline uint8_t tu_rs_msg_proto(const uint8_t *m)
{
	return m[0];
}
static inline uint8_t tu_rs_msg_id(const uint8_t *m)
{
	return m[1];
}
static inline uint16_t tu_rs_msg_len(const uint8_t *m)
{
	return (uint16_t)(((uint16_t)m[2] << 8) | m[3]);
}

#endif /* TINYUWB_SETUP_H */
