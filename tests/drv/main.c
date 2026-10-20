/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file main.c — driver suite entry: the register driver against the fake bus. The engine's
 * two upward calls are recorded here instead of linking round.c. */
#include <stdio.h>

#include "test.h"
#include "tinyuwb/hw.h"

unsigned up_rx_calls, up_tx_calls;
uint32_t up_rx_flags;
uint16_t up_rx_len;

void tu_round_rx_ok(uint32_t flags, uint16_t len)
{
	up_rx_calls++;
	up_rx_flags = flags;
	up_rx_len = len;
}

void tu_round_tx_done(void)
{
	up_tx_calls++;
}

void test_dw3000(void);

int main(void)
{
	test_dw3000();
	printf("%-8s %s\n", "dw3000", t_fail == 0 ? "ok" : "FAIL");
	printf("%d passed, %d failed, %d to record\n", t_pass, t_fail, t_pending);
	return (t_fail == 0 && t_pending == 0) ? 0 : 1;
}
