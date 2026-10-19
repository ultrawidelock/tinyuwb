/** @file test_gate.c — the listen gate: stop kills every RX self-rearm path, start reopens
 * it, and the radio's last act on stop is the TRX off. The driver's callbacks are invoked
 * directly, playing the role of its interrupt work. */
#include <errno.h>
#include <string.h>

#include "hw_fake.h"
#include "test.h"
#include "tinyuwb/hw.h"
#include "tinyuwb/uwb.h"

static int start(uint8_t channel)
{
	static uint8_t ursk[32];
	struct tu_cfg c;

	memset(&c, 0, sizeof(c));
	c.session_id = 1u;
	c.channel = channel;
	c.sync_code_index = 9u;
	c.slot_per_round = 12u;
	c.ursk = ursk;
	return tu_start(&c);
}

void test_gate(void)
{
	unsigned before;

	t_group("stop before any start is a no-op on the bus, and still sleeps the part");
	hw_fake_reset();
	tu_stop();
	T_EQ("stop.cold.no_trxoff", hw.trx_off_calls, 0);
	T_EQ("stop.cold.no_rxenable", hw.rx_enable_calls, 0);
	T_EQ("stop.cold.sleeps", hw.sleep_calls, 1);

	t_group("start arms RX immediately");
	hw_fake_reset();
	T_EQ("start.rc", start(9u), 0);
	T_EQ("start.listen", hw.listen_calls, 1);
	T_EQ("start.armed", hw.rx_enable_calls, 1);
	T_OK("start.immediate", !hw.last_rx_delayed);
	T_OK("start.listening", tu_listening());

	t_group("while up, every RX outcome self-rearms");
	before = hw.rx_enable_calls;
	tu_round_rx_event(0u, 0u); /* quiet event: SP0 fallback */
	T_EQ("up.quiet.rearms", hw.rx_enable_calls, before + 1);
	tu_round_rx_event(TU_RX_TO, 0u);
	T_EQ("up.to.rearms", hw.rx_enable_calls, before + 2);
	tu_round_rx_event(TU_RX_ERR, 0u);
	T_EQ("up.err.rearms", hw.rx_enable_calls, before + 3);

	t_group("stop closes the gate: no path re-enables RX");
	before = hw.trx_off_calls;
	tu_stop();
	T_EQ("stop.trxoff", hw.trx_off_calls, before + 1);
	T_EQ("stop.sleeps", hw.sleep_calls, 1);
	T_OK("stop.not_listening", !tu_listening());
	before = hw.rx_enable_calls;
	tu_round_rx_event(0u, 0u);
	tu_round_rx_event(TU_RX_FCG | TU_RX_PHD | TU_RX_CIA, 0u);
	tu_round_rx_event(TU_RX_TO, 0u);
	tu_round_rx_event(TU_RX_ERR, 0u);
	T_EQ("stop.rx.gated", hw.rx_enable_calls, before);

	t_group("late TX-done after stop cannot rearm the Final window");
	before = hw.rx_enable_calls;
	tu_round_tx_done();
	T_EQ("stop.txdone.gated", hw.rx_enable_calls, before);

	t_group("after stop the radio's last act is the TRX off");
	T_OK("stop.trxoff.last", hw.last_trx_off_seq > hw.last_rx_enable_seq);

	t_group("double stop is idempotent on the bus");
	before = hw.trx_off_calls;
	tu_stop();
	T_EQ("stop.twice.no_trxoff", hw.trx_off_calls, before);

	t_group("resume: start reopens the gate and the rearm loop");
	T_EQ("resume.rc", start(9u), 0);
	before = hw.rx_enable_calls;
	T_OK("resume.armed", before > 0);
	tu_round_rx_event(0u, 0u);
	T_EQ("resume.quiet.rearms", hw.rx_enable_calls, before + 1);

	t_group("start fails when the radio cannot come up");
	tu_stop();
	hw.listen_ret = -5;
	T_EQ("start.radiofail", start(9u), -5);
	T_OK("start.radiofail.not_listening", !tu_listening());
	hw.listen_ret = 0;
	tu_stop();
}
