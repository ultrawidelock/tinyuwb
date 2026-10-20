/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file hw_fake.c — recording double for tinyuwb/hw.h. */
#include "hw_fake.h"

#include <string.h>

#include "tinyuwb/hw.h"

struct hw_fake hw;

void hw_fake_reset(void)
{
	memset(&hw, 0, sizeof(hw));
}

int tu_hw_listen_start(uint8_t channel, uint8_t code)
{
	(void)channel;
	(void)code;
	hw.listen_calls++;
	return hw.listen_ret;
}

void tu_hw_sleep(void)
{
	hw.sleep_calls++;
}

int tu_hw_rx_enable(bool delayed, uint32_t at_hi32, uint32_t timeout)
{
	hw.rx_enable_calls++;
	hw.last_rx_delayed = delayed;
	hw.last_rx_at = at_hi32;
	hw.last_rx_timeout = timeout;
	hw.last_rx_enable_seq = ++hw.seq;
	return hw.rx_enable_ret;
}

void tu_hw_trx_off(void)
{
	hw.trx_off_calls++;
	hw.last_trx_off_seq = ++hw.seq;
}

int tu_hw_tx_delayed(const uint8_t *data, uint16_t len, bool ranging, uint32_t at_hi32)
{
	(void)data;
	(void)len;
	(void)ranging;
	hw.tx_calls++;
	hw.last_tx_at = at_hi32;
	hw.seq++;
	return hw.tx_ret;
}

void tu_hw_sts_key(const uint8_t dursk[16])
{
	memcpy(hw.last_key, dursk, 16);
	hw.key_calls++;
}

void tu_hw_sts_iv(const uint8_t sts_v[16])
{
	memcpy(hw.last_iv, sts_v, 16);
	hw.iv_calls++;
}

void tu_hw_sts_mode(bool sp3)
{
	hw.sts_sp3 = sp3;
}

int tu_hw_sts_quality(int16_t *index)
{
	if (index != NULL) {
		*index = hw.stsq_val;
	}
	return hw.stsq_ret;
}

void tu_hw_rx_read(uint8_t *buf, uint16_t len)
{
	memset(buf, 0, len);
	if (len > sizeof(hw.rxdata)) {
		len = sizeof(hw.rxdata);
	}
	memcpy(buf, hw.rxdata, len);
}

uint64_t tu_hw_rx_timestamp(void)
{
	return hw.rx_ts40;
}

uint64_t tu_hw_tx_timestamp(void)
{
	return hw.tx_ts40;
}

uint32_t tu_hw_time_hi32(void)
{
	return hw.systime;
}
