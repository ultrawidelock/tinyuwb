/** @file test_dw3000.c — the register driver against the fake bus: every transaction the
 * manual prescribes, in the order it prescribes it, and every refusal. */
#include <errno.h>
#include <string.h>

#include "bus_fake.h"
#include "test.h"
#include "tinyuwb/hw.h"
#include "tinyuwb/bus.h"

extern unsigned up_rx_calls, up_tx_calls;
extern uint32_t up_rx_flags;
extern uint16_t up_rx_len;

/* Register positions, written out again here rather than shared with the driver. */
#define SYS_CFG 0x00, 0x10
#define SYS_TIME 0x00, 0x1C
#define TX_FCTRL 0x00, 0x24
#define DX_TIME 0x00, 0x2C
#define RX_FWTO 0x00, 0x34
#define SYS_ENABLE 0x00, 0x3C
#define SYS_STATUS 0x00, 0x44
#define RX_FINFO 0x00, 0x4C
#define TX_TIME 0x00, 0x74
#define TX_ANTD 0x01, 0x04
#define TX_POWER 0x01, 0x0C
#define CHAN_CTRL 0x01, 0x14
#define STS_CFG 0x02, 0x00
#define STS_STS 0x02, 0x08
#define STS_KEY 0x02, 0x0C
#define STS_IV 0x02, 0x1C
#define DGC_CFG 0x03, 0x18
#define DGC_LUT0 0x03, 0x38
#define RX_CAL 0x04, 0x0C
#define RX_CAL_STS 0x04, 0x20
#define DTUNE0 0x06, 0x00
#define RX_SFD_TOC 0x06, 0x02
#define DTUNE3 0x06, 0x0C
#define RF_TX_CTRL_2 0x07, 0x1C
#define PLL_CFG 0x09, 0x00
#define PLL_CAL 0x09, 0x08
#define XTAL 0x09, 0x14
#define AON_CTRL 0x0A, 0x04
#define AON_CFG 0x0A, 0x14
#define OTP_CFG 0x0B, 0x08
#define IP_TS 0x0C, 0x00
#define CIA_CONF 0x0E, 0x00
#define SEQ_CTRL 0x11, 0x08

#define CMD_TXRXOFF 0x00
#define CMD_RX 0x02
#define CMD_DTX 0x03
#define CMD_DRX 0x04
#define CMD_CLR_IRQS 0x12

static void fresh(void)
{
	bf_reset();
	up_rx_calls = up_tx_calls = 0u;
	up_rx_flags = 0u;
	up_rx_len = 0u;
	/* Put the part to sleep so the driver's state is cleared between groups; the first
	 * listen then rebuilds it. */
	tu_hw_sleep();
	bf_reset();
}

static int up(uint8_t channel, uint8_t code)
{
	int rc = tu_hw_listen_start(channel, code);

	bf.nlog = 0u; /* the groups below look only at what follows */
	return rc;
}

static void t_bringup(void)
{
	uint32_t a, o;

	t_group("bring-up");
	fresh();
	bf.otp[0x1E] = 0x2Eu;         /* crystal trim */
	bf.otp[0x04] = 0x11223344u;   /* LDO tune present */
	T_EQ("listen ok", tu_hw_listen_start(9u, 9u), 0);
	T_EQ("one reset", bf.resets, 1);
	T_OK("DEV_ID read", bf_reads(0x00, 0x00) >= 1u);
	T_EQ("xtal trim from OTP", bf_last_write(XTAL), 0x2Eu);
	T_EQ("LDO kick", bf_writes(OTP_CFG) >= 2u, 1);
	T_EQ("CHAN_CTRL ch9 code 9", bf_last_write(CHAN_CTRL), 0x0949u);
	T_EQ("PLL_CFG ch9", bf_last_write(PLL_CFG), 0x0F3Cu);
	T_EQ("RF_TX_CTRL_2 ch9", bf_last_write(RF_TX_CTRL_2), 0x1C010034u);
	T_EQ("TX_POWER", bf_last_write(TX_POWER), 0xFDFDFDFDu);
	T_EQ("DGC LUT0 ch9 (no OTP tuning)", bf_last_write(DGC_LUT0), 0x0002A8FEu);
	T_EQ("DGC_CFG masked", bf_last_masked(DGC_CFG, &a, &o), 2);
	T_EQ("DGC_CFG THR_64 and enable", o, (0x32u << 9) | 1u);
	T_EQ("DTUNE0 masked", bf_last_masked(DTUNE0, &a, &o), 2);
	T_EQ("DTUNE0 PAC 8", (a & 0x13u) | o, 0u);
	T_EQ("SFD timeout 4096 + 64", bf_last_write(RX_SFD_TOC), 4160u);
	T_EQ("DTUNE3", bf_last_write(DTUNE3), 0xAF5F35CCu);
	T_EQ("SYS_CFG masked", bf_last_masked(SYS_CFG, &a, &o), 3);
	T_EQ("SYS_CFG set bits", o, (1u << 3) | (1u << 7) | (1u << 8));
	T_EQ("SYS_CFG cleared CP_SPC", a & (3u << 12), 0u);
	T_EQ("STS length 64", bf_last_write(STS_CFG), 7u);
	T_EQ("TX_ANTD", bf_last_write(TX_ANTD), 0x4015u);
	T_EQ("CIA_CONF masked", bf_last_masked(CIA_CONF, &a, &o), 3);
	T_EQ("RX_ANTD", o, 0x4015u);
	T_EQ("PLL_CAL", bf_last_write(PLL_CAL), 0x0181u);
	T_EQ("SEQ_CTRL AINIT2IDLE", bf_last_masked(SEQ_CTRL, &a, &o) == 3 && o == (1u << 8), 1);
	T_EQ("RX cal mode and enable cleared at the end", bf_last_write(RX_CAL), 0u);
	T_EQ("RX cal status cleared", bf_last_write(RX_CAL_STS), 1u);
	T_EQ("SYS_ENABLE", bf_last_write(SYS_ENABLE) & (1u << 7), 1u << 7);
	T_EQ("CLR_IRQS", bf.cmds[CMD_CLR_IRQS], 1);
	T_EQ("irq enabled", bf.irq_on, 1);
	{
		/* The CPLOCK clear must land before the sequencer is released. */
		int i_clear = -1, i_seq = -1;

		for (unsigned i = 0u; i < bf.nlog; i++) {
			const struct bf_op *op = &bf.log[i];

			if (op->wr && !op->fast && op->mode == 0u && op->file == 0x00 &&
			    op->off == 0x44 && (op->data[0] & 0x02u) != 0u && i_clear < 0) {
				i_clear = (int)i;
			}
			if (op->wr && !op->fast && op->mode != 0u && op->file == 0x11 &&
			    op->off == 0x08) {
				i_seq = (int)i;
			}
		}
		T_OK("CPLOCK cleared before the sequencer runs", i_clear >= 0 && i_seq > i_clear);
	}

	bf.nlog = 0u;
	T_EQ("same PHY again", tu_hw_listen_start(9u, 9u), 0);
	T_EQ("no second reset", bf.resets, 1);
	T_EQ("no reconfigure", bf_writes(PLL_CFG), 0u);
	T_EQ("TRX off", bf.cmds[CMD_TXRXOFF] >= 1u, 1);

	bf.nlog = 0u;
	T_EQ("new code", tu_hw_listen_start(9u, 11u), 0);
	T_EQ("still no reset", bf.resets, 1);
	T_EQ("CHAN_CTRL ch9 code 11", bf_last_write(CHAN_CTRL), 0x0B59u);

	bf.nlog = 0u;
	T_EQ("channel 5", tu_hw_listen_start(5u, 9u), 0);
	T_EQ("CHAN_CTRL ch5", bf_last_write(CHAN_CTRL), 0x0948u);
	T_EQ("PLL_CFG ch5", bf_last_write(PLL_CFG), 0x1F3Cu);
	T_EQ("RF_TX_CTRL_2 ch5", bf_last_write(RF_TX_CTRL_2), 0x1C071134u);
	T_EQ("DGC LUT0 ch5", bf_last_write(DGC_LUT0), 0x0001C0FDu);

	T_EQ("bad channel", tu_hw_listen_start(7u, 9u), -EINVAL);
}

static void t_bringup_otp_dgc(void)
{
	uint32_t a, o;

	t_group("bring-up with OTP tuning");
	fresh();
	bf.otp[0x20] = 0x10000240u;
	T_EQ("listen ok", tu_hw_listen_start(9u, 9u), 0);
	T_EQ("no LUT writes", bf_writes(DGC_LUT0), 0u);
	T_EQ("DGC kick for channel 9", bf_last_write(OTP_CFG) == 0u, 1);
	T_OK("kick word seen", bf_writes(OTP_CFG) >= 2u);
	T_EQ("xtal default when OTP blank", bf_last_write(XTAL), 0x20u);
	T_EQ("DGC_CFG still tuned", bf_last_masked(DGC_CFG, &a, &o), 2);
}

static void t_bringup_failures(void)
{
	t_group("bring-up refusals");
	fresh();
	bf.devid = 0x12345678u;
	T_EQ("wrong part", tu_hw_listen_start(9u, 9u), -ENODEV);
	fresh();
	bf.no_cplock = true;
	T_EQ("PLL never locks", tu_hw_listen_start(9u, 9u), -EIO);
	fresh();
	bf.rx_cal_fail = true;
	T_EQ("RX calibration fails", tu_hw_listen_start(9u, 9u), -EIO);
	T_EQ("irq stays off", bf.irq_on, 0);
}

static void t_rx(void)
{
	uint32_t a, o;

	t_group("rx enable");
	fresh();
	T_EQ("up", up(9u, 9u), 0);
	T_EQ("immediate, no timeout", tu_hw_rx_enable(false, 0u, 0u), 0);
	T_EQ("CMD_RX", bf.cmds[CMD_RX], 1);
	T_EQ("RXWTOE cleared", bf_last_masked(0x00, 0x11, &a, &o) == 1 && (a & 0x02u) == 0u, 1);

	bf.nlog = 0u;
	T_EQ("immediate with timeout", tu_hw_rx_enable(false, 0u, 0x12345u), 0);
	T_EQ("RX_FWTO", bf_last_write(RX_FWTO), 0x012345u);
	T_EQ("RXWTOE set", bf_last_masked(0x00, 0x11, &a, &o) == 1 && (o & 0x02u) != 0u, 1);

	bf.nlog = 0u;
	T_EQ("delayed on time", tu_hw_rx_enable(true, 0x11223344u, 1350u), 0);
	T_EQ("DX_TIME", bf_last_write(DX_TIME), 0x11223344u);
	T_EQ("CMD_DRX", bf.cmds[CMD_DRX], 1);

	bf.nlog = 0u;
	bf.hpdwarn = true;
	T_EQ("delayed late", tu_hw_rx_enable(true, 0x11223344u, 1350u), -ETIME);
	T_EQ("TRX off after the late start", bf.cmds[CMD_TXRXOFF] >= 1u, 1);
	T_EQ("HPDWARN cleared", bf_reg32(SYS_STATUS) & (1u << 27), 0u);

	tu_hw_trx_off();
	T_EQ("trx_off is the fast command", bf.cmds[CMD_TXRXOFF] >= 2u, 1);
}

static void t_tx(void)
{
	static const uint8_t frame[] = {0x41, 0x88, 0x00, 0xAD, 0xDE, 0x01, 0x02, 0x03, 0x04};
	uint8_t big[126];

	t_group("tx delayed");
	fresh();
	T_EQ("up", up(9u, 9u), 0);
	T_EQ("tx ok", tu_hw_tx_delayed(frame, sizeof(frame), true, 0x0ABCDEF0u), 0);
	T_EQ("buffer bytes", memcmp(&bf.img[0x14][0], frame, sizeof(frame)), 0);
	T_EQ("TX_FCTRL len+2, 6M8, ranging, 64-symbol preamble", bf_last_write(TX_FCTRL),
	     (uint32_t)(sizeof(frame) + 2u) | (1u << 10) | (1u << 11) | (1u << 12));
	T_EQ("DX_TIME", bf_last_write(DX_TIME), 0x0ABCDEF0u);
	T_EQ("CMD_DTX", bf.cmds[CMD_DTX], 1);

	bf.nlog = 0u;
	T_EQ("no ranging bit", tu_hw_tx_delayed(frame, 4u, false, 1u), 0);
	T_EQ("TX_FCTRL", bf_last_write(TX_FCTRL), 6u | (1u << 10) | (1u << 12));

	bf.hpdwarn = true;
	T_EQ("late", tu_hw_tx_delayed(frame, 4u, true, 1u), -ETIME);
	memset(big, 0, sizeof(big));
	T_EQ("too long", tu_hw_tx_delayed(big, sizeof(big), true, 1u), -EINVAL);
}

static void t_sts(void)
{
	static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
					0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
	static const uint8_t iv[16] = {0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87,
				       0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F};
	uint8_t rev[16];
	uint32_t a, o;
	int16_t q;

	t_group("sts");
	fresh();
	T_EQ("up", up(9u, 9u), 0);
	tu_hw_sts_key(key);
	for (unsigned i = 0u; i < 16u; i++) {
		rev[i] = key[15u - i];
	}
	T_EQ("key image reversed", memcmp(&bf.img[0x02][0x0C], rev, 16u), 0);
	T_EQ("one key write", bf_writes(STS_KEY), 1u);
	tu_hw_sts_key(key);
	T_EQ("same key skipped", bf_writes(STS_KEY), 1u);
	tu_hw_sts_key(iv);
	T_EQ("new key written", bf_writes(STS_KEY), 2u);

	tu_hw_sts_iv(iv);
	for (unsigned i = 0u; i < 16u; i++) {
		rev[i] = iv[15u - i];
	}
	T_EQ("iv image reversed", memcmp(&bf.img[0x02][0x1C], rev, 16u), 0);
	T_EQ("iv latched", bf.iv_loads, 1);

	bf.nlog = 0u;
	tu_hw_sts_mode(true);
	T_EQ("CP_SPC = 3", bf_last_masked(0x00, 0x11, &a, &o) == 1 && o == 0x30u, 1);
	T_EQ("SYS_CFG holds SP3", bf_reg32(SYS_CFG) & (3u << 12), 3u << 12);
	T_EQ("iv latched again after the mode change", bf.iv_loads, 2);
	bf.nlog = 0u;
	tu_hw_sts_mode(true);
	T_EQ("no-op when unchanged", bf.nlog, 0);
	tu_hw_sts_mode(false);
	T_EQ("CP_SPC = 0", bf_reg32(SYS_CFG) & (3u << 12), 0u);
	T_EQ("no latch on the way back", bf.iv_loads, 2);

	bf_set32(STS_STS, 50u);
	T_EQ("quality 50 is good", tu_hw_sts_quality(&q) >= 0, 1);
	T_EQ("index 50", q, 50);
	bf_set32(STS_STS, 38u);
	T_EQ("quality 38 is poor", tu_hw_sts_quality(&q) < 0, 1);
	bf_set32(STS_STS, 0x0800u);
	T_EQ("sign extension", tu_hw_sts_quality(&q) < 0 && q < 0, 1);

	T_EQ("sleep drops the key cache", (tu_hw_sleep(), bf_last_write(AON_CFG)), 0x19u);
	T_EQ("AON upload", bf_last_write(AON_CTRL), 0x04u);
	T_EQ("irq off", bf.irq_off >= 1u, 1);
	bf.nlog = 0u;
	T_EQ("listen rebuilds", tu_hw_listen_start(9u, 9u), 0);
	T_EQ("reset on wake", bf.resets, 2);
	tu_hw_sts_key(iv);
	T_EQ("key written again", bf_writes(STS_KEY), 1u);
}

static void t_time(void)
{
	uint8_t ts[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x00, 0x00, 0x80};

	t_group("timestamps");
	fresh();
	T_EQ("up", up(9u, 9u), 0);
	memcpy(&bf.img[0x0C][0x00], ts, 8u);
	T_OK("rx timestamp 40 bits", tu_hw_rx_timestamp() == 0x5544332211ull);
	memcpy(&bf.img[0x00][0x74], ts, 5u);
	T_OK("tx timestamp 40 bits", tu_hw_tx_timestamp() == 0x5544332211ull);
	bf.sys_time = 0x10000000u;
	T_EQ("time hi32 is fresh", tu_hw_time_hi32(), 0x10001000u);
	T_EQ("and fresh again", tu_hw_time_hi32(), 0x10002000u);
	T_EQ("SYS_TIME released by a write", bf_writes(SYS_TIME), 2u);

	memset(&bf.img[0x12][0], 0xA5, 8u);
	bf.img[0x12][3] = 0x5A;
	{
		uint8_t b[8] = {0};

		tu_hw_rx_read(b, 8u);
		T_EQ("rx read", b[3], 0x5A);
		T_EQ("rx read 0", b[0], 0xA5);
	}
}

static void t_irq(void)
{
	t_group("irq");
	fresh();
	tu_dw3000_irq();
	T_EQ("nothing before bring-up", up_rx_calls + up_tx_calls, 0u);
	T_EQ("up", up(9u, 9u), 0);

	bf_set32(SYS_STATUS, (1u << 7) | (1u << 4));
	tu_dw3000_irq();
	T_EQ("tx done", up_tx_calls, 1u);
	T_EQ("tx bits cleared", bf_reg32(SYS_STATUS) & 0xF0u, 0u);
	T_EQ("no rx event", up_rx_calls, 0u);

	bf_set32(SYS_STATUS, (1u << 14) | (1u << 13) | (1u << 11) | (1u << 10));
	bf_set32(RX_FINFO, 0x12300017u);
	tu_dw3000_irq();
	T_EQ("rx good", up_rx_calls, 1u);
	T_EQ("flags FCG PHD CIA", up_rx_flags, TU_RX_FCG | TU_RX_PHD | TU_RX_CIA);
	T_EQ("len from RX_FINFO", up_rx_len, 0x17);
	T_EQ("rx bits cleared", bf_reg32(SYS_STATUS) & 0x3FFFFF00u, 0u);

	bf_set32(SYS_STATUS, (1u << 13) | (1u << 10));
	tu_dw3000_irq();
	T_EQ("RXFR alone in SP0 is not a frame", up_rx_calls, 1u);
	T_EQ("but is cleared", bf_reg32(SYS_STATUS) & (1u << 13), 0u);

	tu_hw_sts_mode(true);
	bf_set32(SYS_STATUS, (1u << 13) | (1u << 10) | (1u << 28));
	tu_dw3000_irq();
	T_EQ("SP3 frame on RXFR", up_rx_calls, 2u);
	T_EQ("flags CIA CPER, no FCG", up_rx_flags, TU_RX_CIA | TU_RX_CPER);
	T_EQ("len 0", up_rx_len, 0);
	tu_hw_sts_mode(false);

	bf_set32(SYS_STATUS, 1u << 17);
	tu_dw3000_irq();
	T_EQ("timeout", up_rx_flags, TU_RX_TO);
	bf_set32(SYS_STATUS, 1u << 21);
	tu_dw3000_irq();
	T_EQ("preamble timeout", up_rx_flags, TU_RX_TO);

	bf_set32(SYS_STATUS, (1u << 15) | (1u << 11));
	bf_set32(RX_FINFO, 0x9u);
	tu_dw3000_irq();
	T_EQ("crc error with header", up_rx_flags, TU_RX_ERR | TU_RX_PHD);
	T_EQ("header length", up_rx_len, 9);
	bf_set32(SYS_STATUS, 1u << 26);
	tu_dw3000_irq();
	T_EQ("sfd timeout", up_rx_flags, TU_RX_ERR);
	T_EQ("six rx events", up_rx_calls, 6u);

	bf_set32(SYS_STATUS, 1u << 27);
	tu_dw3000_irq();
	T_EQ("HPDWARN alone is not an rx event", up_rx_calls, 6u);
	T_EQ("and is left for the arm path", bf_reg32(SYS_STATUS) & (1u << 27), 1u << 27);
}

static void t_headers(void)
{
	t_group("spi headers");
	fresh();
	T_EQ("up", up(9u, 9u), 0);
	bf.nlog = 0u;
	tu_hw_rx_read((uint8_t[4]){0}, 4u);
	T_EQ("one-octet read header for file 0x12 offset 0", bf.log[0].file == 0x12 &&
	     bf.log[0].off == 0 && !bf.log[0].wr, 1);
	T_EQ("dropped nothing", bf.dropped, 0u);
}

void test_dw3000(void)
{
	t_headers();
	t_bringup();
	t_bringup_otp_dgc();
	t_bringup_failures();
	t_rx();
	t_tx();
	t_sts();
	t_time();
	t_irq();
}
