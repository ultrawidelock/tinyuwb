/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file hw_fake.h — recording double for tinyuwb/hw.h. Every call is counted and
 * sequenced so a test can assert the listen-gate semantics and the arm modes. */
#ifndef TINYUWB_HW_FAKE_H
#define TINYUWB_HW_FAKE_H

#include <stdbool.h>
#include <stdint.h>

struct hw_fake {
	int listen_ret;      /* tu_hw_listen_start() result */
	int rx_enable_ret;   /* tu_hw_rx_enable() result */
	int tx_ret;          /* tu_hw_tx_delayed() result */
	unsigned listen_calls, sleep_calls, rx_enable_calls, trx_off_calls, tx_calls;
	bool last_rx_delayed;
	uint32_t last_rx_at, last_rx_timeout, last_tx_at;
	unsigned seq, last_rx_enable_seq, last_trx_off_seq;
	bool sts_sp3;
	uint8_t last_key[16], last_iv[16];
	unsigned key_calls, iv_calls;
	int stsq_ret;
	int16_t stsq_val;
	uint8_t rxdata[160];
	uint16_t rxdata_len;
	uint64_t rx_ts40, tx_ts40;
	uint32_t systime;
};

extern struct hw_fake hw;

void hw_fake_reset(void);

#endif /* TINYUWB_HW_FAKE_H */
