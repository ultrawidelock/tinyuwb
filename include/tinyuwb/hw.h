/** @file hw.h — the radio the round engine drives: the whole driver contract.
 *
 * Implemented by the DW3000 driver on the target and by
 * tests/host/hw_fake.c on the host. Nothing above this header names a register.
 * Times tagged hi32 are the upper 32 bits of the 40-bit device time (1 unit =
 * 256 device time units, about 4 ns); timeouts are in the chip's frame-wait
 * unit (about 1.0256 us); timestamps are the full 40-bit device time.
 */

#ifndef TINYUWB_HW_H
#define TINYUWB_HW_H

#include <stdbool.h>
#include <stdint.h>

/* RX event flags the driver hands to tu_round_rx_event() / tu_round_rx_ok(). */
#define TU_RX_FCG  0x01u /**< frame good: CRC passed, bytes readable */
#define TU_RX_PHD  0x02u /**< PHR decoded: the header bytes are readable even on a CRC fail */
#define TU_RX_CIA  0x04u /**< CIA done: the RX timestamp is valid */
#define TU_RX_CPER 0x08u /**< STS correlation error: the timestamp is not authenticated */
#define TU_RX_TO   0x10u /**< frame-wait timeout, no frame */
#define TU_RX_ERR  0x20u /**< any other receive error */

/** @brief Bring the radio up for the SP0 Pre-POLL listen on @p channel with preamble code
 * @p code (64-symbol preamble, PAC 8, IEEE 4a SFD, 6.8 Mbps, standard PHR, STS off, STS
 * length 64), TRX off, RX not armed. Cached: a second call with the same parameters
 * since the last tu_hw_sleep() skips the configure. @return 0, or a negative errno. */
int tu_hw_listen_start(uint8_t channel, uint8_t code);

/** @brief Put the part to sleep and forget the cached PHY; safe when never started. */
void tu_hw_sleep(void);

/** @brief Enable RX now, or at @p at_hi32 when @p delayed. @p timeout 0 = no frame-wait
 * timeout. A delayed start already in the past is refused with -ETIME and leaves the
 * receiver off. */
int tu_hw_rx_enable(bool delayed, uint32_t at_hi32, uint32_t timeout);

/** @brief Force the transceiver off, clearing any pending delayed start. */
void tu_hw_trx_off(void);

/** @brief Transmit @p len bytes (FCS appended by the chip) at @p at_hi32, with the ranging
 * bit set when @p ranging. @return 0, or -ETIME when the start time has passed. */
int tu_hw_tx_delayed(const uint8_t *data, uint16_t len, bool ranging, uint32_t at_hi32);

/** @brief Load the 16-byte STS key (dURSK). The driver may skip the write when unchanged
 * since its last load; tu_hw_listen_start() and tu_hw_sleep() drop that memory. */
void tu_hw_sts_key(const uint8_t dursk[16]);

/** @brief Load the 16-byte STS IV (STS-V) and latch it into the STS counter. */
void tu_hw_sts_iv(const uint8_t sts_v[16]);

/** @brief Select SP3 no-data (STS on) when @p sp3, else SP0 (STS off). */
void tu_hw_sts_mode(bool sp3);

/** @brief STS quality of the last reception: writes the signed index, returns >= 0 when the
 * STS correlated, < 0 when it did not. */
int tu_hw_sts_quality(int16_t *index);

/** @brief Copy @p len bytes of the last received frame from offset 0. */
void tu_hw_rx_read(uint8_t *buf, uint16_t len);

/** @brief The last reception's 40-bit Ipatov RX timestamp. */
uint64_t tu_hw_rx_timestamp(void);

/** @brief The last transmission's 40-bit TX timestamp. */
uint64_t tu_hw_tx_timestamp(void);

/** @brief Current device time, upper 32 bits. */
uint32_t tu_hw_time_hi32(void);

/* ── Upward: what the driver calls from its interrupt work ───────────────── */

/** @brief A receive outcome: good frame, timeout or error, with @p len = frame length
 * including the 2 FCS bytes (0 when none). The driver calls this for every RX event. */
void tu_round_rx_ok(uint32_t flags, uint16_t len);

/** @brief Our transmission left the antenna. */
void tu_round_tx_done(void);

#endif /* TINYUWB_HW_H */
