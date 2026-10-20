/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file dw3000.c — DW3000 register driver behind tinyuwb/hw.h.
 *
 * References: DW3000 Family User Manual 1.1 (register files, chapter 8; fast commands,
 * chapter 9; start-up and states, chapter 2; delayed TX and RX, 3.3 and 4.3; STS, chapter 6)
 * and the DW3000 Data Sheet 1.3 (SPI transaction formats, 4.9.4).
 * Choices the manual leaves open are marked REVIEW.
 *
 * One chip (DW3110 in the DWM3001C), one PHY set: 64-symbol preamble, PAC 8, IEEE 4a SFD,
 * 6.8 Mbit/s, standard PHR at 850 kbit/s, STS length 64, PDoA off, single RX buffer, frame
 * filtering off. The only per-frame switch is SP0 against SP3 (SYS_CFG.CP_SPC).
 *
 * Time units: the chip's 40-bit time counts 499.2 MHz x 128; DX_TIME, SYS_TIME and the
 * engine's hi32 values are its upper 32 bits (about 4 ns per unit). RX_FWTO counts
 * 512 / 499.2 MHz (about 1.0256 us).
 */

#include "tinyuwb/hw.h"

#include "tinyuwb/bus.h"
#include "dw3000_regs.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef TU_TX_POWER
#define TU_TX_POWER 0xFDFDFDFDu /**< TX_POWER, all four stages (bench value for channel 9) */
#endif
#ifndef TU_TX_ANTD
#define TU_TX_ANTD 0x4015u /**< TX antenna delay, chip default (8.2.2.19) */
#endif
#ifndef TU_RX_ANTD
#define TU_RX_ANTD 0x4015u /**< RX antenna delay, chip default (8.2.13.36) */
#endif
#ifndef TU_XTAL_TRIM_DEFAULT
#define TU_XTAL_TRIM_DEFAULT 0x20u /**< mid-scale when the OTP holds no trim (REVIEW) */
#endif
/* ACC_QUAL below 60 % of the STS length means the STS timestamp is not trustworthy (manual
 * chapter 6); the length is 64 so the first trusted value is 39. */
#define TU_STS_QUAL_MIN 39

#define DW_DEVID_FAMILY 0xDECA0300u
#define DW_RX_CAL_FAILED 0x1FFFFFFFu

/* SYS_ENABLE low word: every event the ISR dispatches on. SPIRDY is on by default at reset and
 * is masked here so an idle part does not hold the line. */
#define DW_IRQ_MASK                                                                           \
	(DW_ST_TXFRS | DW_ST_RXFR | DW_ST_RXFCG | DW_ST_RXFTO | DW_ST_RXPTO | DW_ST_RXPHE |   \
	 DW_ST_RXFCE | DW_ST_RXFSL | DW_ST_RXSTO | DW_ST_ARFE | DW_ST_CIAERR | DW_ST_RXOVRR | \
	 DW_ST_CPERR)

static struct {
	bool up;        /* reset, trimmed, PLL locked, receiver calibrated */
	bool phy_valid; /* phy_channel and phy_code are what the chip holds */
	uint8_t phy_channel, phy_code;
	bool sp3;
	bool key_valid;
	uint8_t key[16]; /* STS_KEY image as written */
	uint32_t otp_dgc0; /* OTP word 0x20, 0 when the part carries no DGC calibration */
	struct tu_hw_stats st;
	uint32_t otp[5];
} g;

void tu_hw_otp_get(uint32_t out[5])
{
	memcpy(out, g.otp, sizeof(g.otp));
}

void tu_hw_stats_get(struct tu_hw_stats *out)
{
	*out = g.st;
}

/* ── SPI transactions (data sheet 4.9.4, manual 2.3.1.2) ────────────────── */

/* Header octet 0: bit 7 write, bit 6 two-octet header, bits 5:1 file ID, bit 0 is the
 * sub-address's bit 6 (two-octet) or 0 (one-octet). Octet 1: bits 7:2 sub-address bits 5:0,
 * bits 1:0 the mode: 00 plain, 01 / 10 / 11 masked write of 1 / 2 / 4 octets. */
static uint8_t hdr_build(uint8_t h[2], bool wr, uint8_t file, uint8_t off, uint8_t mode)
{
	h[0] = (uint8_t)((wr ? 0x80u : 0u) | ((file & 0x1Fu) << 1));
	if (off == 0u && mode == 0u) {
		return 1u;
	}
	h[0] |= (uint8_t)(0x40u | ((off >> 6) & 1u));
	h[1] = (uint8_t)(((off & 0x3Fu) << 2) | mode);
	return 2u;
}

static int reg_rd(uint8_t file, uint8_t off, uint8_t *buf, uint16_t len)
{
	uint8_t h[2];
	uint8_t n = hdr_build(h, false, file, off, 0u);

	return tu_bus_read(h, n, buf, len);
}

static int reg_wr(uint8_t file, uint8_t off, const uint8_t *data, uint16_t len)
{
	uint8_t h[2];
	uint8_t n = hdr_build(h, true, file, off, 0u);

	return tu_bus_write(h, n, data, len);
}

static uint32_t rd32(uint8_t file, uint8_t off)
{
	uint8_t b[4] = {0};

	(void)reg_rd(file, off, b, 4u);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

static uint16_t rd16(uint8_t file, uint8_t off)
{
	uint8_t b[2] = {0};

	(void)reg_rd(file, off, b, 2u);
	return (uint16_t)(b[0] | (b[1] << 8));
}

static uint8_t rd8(uint8_t file, uint8_t off)
{
	uint8_t b = 0;

	(void)reg_rd(file, off, &b, 1u);
	return b;
}

static uint64_t rd40(uint8_t file, uint8_t off)
{
	uint8_t b[5] = {0};
	uint64_t v = 0;

	(void)reg_rd(file, off, b, 5u);
	for (int i = 4; i >= 0; i--) {
		v = (v << 8) | b[i];
	}
	return v;
}

static void put32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

static void wr32(uint8_t file, uint8_t off, uint32_t v)
{
	uint8_t b[4];

	put32(b, v);
	(void)reg_wr(file, off, b, 4u);
}

static void wr16(uint8_t file, uint8_t off, uint16_t v)
{
	uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};

	(void)reg_wr(file, off, b, 2u);
}

static void wr8(uint8_t file, uint8_t off, uint8_t v)
{
	(void)reg_wr(file, off, &v, 1u);
}

/* Masked write (data sheet 4.9.4 type 4): the chip ANDs then ORs in place, so a read-modify-
 * write costs one transaction. Not for write-1-to-clear status bits. */
static void wr_and_or32(uint8_t file, uint8_t off, uint32_t and_mask, uint32_t or_mask)
{
	uint8_t h[2], d[8];

	(void)hdr_build(h, true, file, off, 3u);
	put32(d, and_mask);
	put32(d + 4, or_mask);
	(void)tu_bus_write(h, 2u, d, 8u);
}

static void wr_and_or8(uint8_t file, uint8_t off, uint8_t and_mask, uint8_t or_mask)
{
	uint8_t h[2], d[2] = {and_mask, or_mask};

	(void)hdr_build(h, true, file, off, 1u);
	(void)tu_bus_write(h, 2u, d, 2u);
}

static void wr_and_or16(uint8_t file, uint8_t off, uint16_t and_mask, uint16_t or_mask)
{
	uint8_t h[2], d[4] = {(uint8_t)and_mask, (uint8_t)(and_mask >> 8), (uint8_t)or_mask,
			      (uint8_t)(or_mask >> 8)};

	(void)hdr_build(h, true, file, off, 2u);
	(void)tu_bus_write(h, 2u, d, 4u);
}

/* Fast command (manual 2.3.1.2.1): bit 7 and bit 0 set, the 5-bit code in bits 5:1. */
static void fast_cmd(uint8_t cmd)
{
	uint8_t h = (uint8_t)(0x81u | ((cmd & 0x1Fu) << 1));

	(void)tu_bus_write(&h, 1u, NULL, 0u);
}

static void status_clear(uint32_t bits)
{
	wr32(DW_SYS_STATUS, bits);
}

/* Poll SYS_STATUS for @p bits, @p polls times @p step_us apart. */
static bool wait_status(uint32_t bits, unsigned polls, uint32_t step_us)
{
	while (polls-- > 0u) {
		if ((rd32(DW_SYS_STATUS) & bits) != 0u) {
			return true;
		}
		tu_bus_delay_us(step_us);
	}
	return false;
}

/* ── OTP (manual 7.3, 8.2.12) ────────────────────────────────────────────── */

/* REVIEW: the manual defers the read sequence to the API. OTP_MAN with OTP_READ, then the
 * word from OTP_RDATA, then the interface released. */
static uint32_t otp_read(uint16_t addr)
{
	uint32_t v;

	wr16(DW_OTP_ADDR, addr);
	wr16(DW_OTP_CFG, 0x0003u);
	v = rd32(DW_OTP_RDATA);
	wr16(DW_OTP_CFG, 0u);
	return v;
}

/* REVIEW: the kick bits start a copy from OTP into the LDO, bias and DGC registers; the
 * manual does not say whether they self-clear, so they are held for 10 us and released. */
static void otp_kick(uint16_t kick_bits)
{
	wr16(DW_OTP_CFG, kick_bits);
	tu_bus_delay_us(10u);
	wr16(DW_OTP_CFG, 0u);
}

/* ── Bring-up (manual 2.4, 2.5, 7.3, 8.2.10, 8.2.12) ────────────────────── */

/* Reset, wait for IDLE_RC, check the part, apply the OTP trims. Leaves the part in IDLE_RC. */
static int chip_init(void)
{
	uint32_t id = 0u, ldo_lo, ldo_hi, bias, xtal;

	memset(&g, 0, sizeof(g));
	tu_bus_reset();
	for (unsigned i = 0u; i < 10u; i++) {
		id = rd32(DW_DEV_ID);
		if ((id & 0xFFFFFF00u) == DW_DEVID_FAMILY) {
			break;
		}
		tu_bus_delay_us(1000u);
	}
	if ((id & 0xFFFFFF00u) != DW_DEVID_FAMILY) {
		return -ENODEV;
	}
	/* INIT_RC ends with RCINIT and SPIRDY in SYS_STATUS (8.2.2.14). */
	if (!wait_status(DW_ST_RCINIT | DW_ST_SPIRDY, 50u, 100u)) {
		return -EIO;
	}

	ldo_lo = otp_read(DW_OTP_LDOTUNE_LO);
	ldo_hi = otp_read(DW_OTP_LDOTUNE_HI);
	bias = otp_read(DW_OTP_BIASTUNE);
	xtal = otp_read(DW_OTP_XTAL_TRIM);
	g.otp_dgc0 = otp_read(DW_OTP_DGC_CFG0);
	g.otp[0] = ldo_lo;
	g.otp[1] = ldo_hi;
	g.otp[2] = bias;
	g.otp[3] = xtal;
	g.otp[4] = g.otp_dgc0;
	if ((ldo_lo | ldo_hi) != 0u || bias != 0u) {
		otp_kick((uint16_t)(((ldo_lo | ldo_hi) != 0u ? (1u << 7) : 0u) |
				    (bias != 0u ? (1u << 8) : 0u)));
	}
	/* Manual 8.2.10.2: a non-zero OTP 0x35 is the PLL calibration's starting code. */
	uint32_t pll_code = otp_read(DW_OTP_PLL_CODE);
	if (pll_code != 0u) {
		wr32(0x09, 0x04, pll_code);
	}
	/* XTAL_TRIM is 6 bits in 8.2.10.4 while the OTP field is 7 (table 17); the OTP width is
	 * kept so a calibrated part is never detuned by truncation (REVIEW). */
	xtal &= 0x7Fu;
	wr8(DW_XTAL, xtal != 0u ? (uint8_t)xtal : (uint8_t)TU_XTAL_TRIM_DEFAULT);
	status_clear(DW_ST_RCINIT | DW_ST_SPIRDY);
	return 0;
}

/* DGC lookup tables, manual table 24 (register file 0x03), for parts without OTP tuning. */
static const uint32_t k_dgc_lut_ch5[7] = {0x0001C0FDu, 0x0001C43Eu, 0x0001C6BEu, 0x0001C77Eu,
					  0x0001CF36u, 0x0001CFB5u, 0x0001CFF5u};
static const uint32_t k_dgc_lut_ch9[7] = {0x0002A8FEu, 0x0002AC36u, 0x0002A5FEu, 0x0002AF3Eu,
					  0x0002AF7Du, 0x0002AFB5u, 0x0002AFB5u};

/* Channel, PHY, PLL lock and receiver calibration. Leaves the part in IDLE_PLL, TRX off. */
static int phy_configure(uint8_t channel, uint8_t code)
{
	const bool ch9 = (channel == 9u);
	uint16_t ldo_ctrl;
	bool cal_ok;

	if (channel != 5u && channel != 9u) {
		return -EINVAL;
	}
	fast_cmd(DW_CMD_TXRXOFF);

	/* Channel and preamble codes (8.2.2.22): RF_CHAN bit 0, SFD_TYPE bits 2:1 = 0 for the
	 * IEEE 802.15.4 8-symbol SFD, TX_PCODE bits 7:3, RX_PCODE bits 12:8. */
	/* SFD_TYPE 0, the IEEE 802.15.4 short 8-symbol SFD: what the reference reader's
	 * Pre-POLL configuration uses (4z timed out on every phone frame there). */
	wr16(DW_CHAN_CTRL, (uint16_t)((ch9 ? 1u : 0u) | ((code & 0x1Fu) << 3) |
				      ((code & 0x1Fu) << 8)));
	/* Analog TX per channel (8.2.8.4, 8.2.8.5, 8.2.8.10). PG_DELAY 0x34 is inside the word. */
	wr32(DW_RF_TX_CTRL_2, ch9 ? 0x1C010034u : 0x1C071134u);
	wr8(DW_RF_TX_CTRL_1, 0x0Eu);
	wr8(DW_LDO_RLOAD, 0x14u);
	wr32(DW_TX_POWER, TU_TX_POWER);
	/* Synthesiser per channel (table 35). */
	wr16(DW_PLL_CFG, ch9 ? 0x0F3Cu : 0x1F3Cu);
	/* Manual 8.2.12.3 and 8.2.12.7: load the short receiver operating parameter set. */
	wr_and_or16(DW_OTP_CFG, (uint16_t)~0x1800u, 0x1000u | 0x0400u);

	/* Receiver gain tables: the part's own OTP calibration when present (DGC_KICK with
	 * DGC_SEL, 8.2.12.3), else the manual's defaults. RX_TUNE_EN is for 64 MHz PRF, which
	 * every code from 9 up selects; THR_64 0x32 per 8.2.4.1. */
	if (g.otp_dgc0 != 0u) {
		otp_kick((uint16_t)((1u << 6) | (ch9 ? (1u << 13) : 0u)));
	} else {
		const uint32_t *lut = ch9 ? k_dgc_lut_ch9 : k_dgc_lut_ch5;

		wr32(DW_DGC_CFG0, 0x10000240u);
		wr32(DW_DGC_CFG1, 0x1B6DA489u);
		for (uint8_t i = 0u; i < 7u; i++) {
			wr32(0x03, (uint8_t)(0x38u + 4u * i), lut[i]);
		}
	}
	wr_and_or16(DW_DGC_CFG, (uint16_t)~((0x3Fu << 9) | 1u), (0x32u << 9) | 1u);

	/* Digital receiver (8.2.7): PAC 8, DT0B4 clear, SFD timeout = 64 + 1 - 8 + 8 symbols,
	 * no preamble timeout, DTUNE3 at the recommended value. */
	wr_and_or16(DW_DTUNE0, (uint16_t)~0x13u, 0u);
	/* REVIEW: 65 (64-symbol preamble + 1) timed out on every frame from a phone with the
	 * preamble detected; the reset default of 4096 + 64 covers any preamble length. */
	wr16(DW_RX_SFD_TOC, 4096u + 64u);
	wr16(DW_PRE_TOC, 0u);
	wr32(DW_DTUNE3, 0xAF5F35CCu);

	/* SYS_CFG (8.2.2.4): standard PHR at 850 kbit/s, SP0, no SDC, PDoA off, filtering off,
	 * no auto re-enable, single buffer, both CIA passes on. */
	wr_and_or32(DW_SYS_CFG,
		    ~(DW_CFG_FFEN | DW_CFG_PHR_MODE | DW_CFG_PHR_6M8 | DW_CFG_RXAUTR |
		      DW_CFG_AUTO_ACK | DW_CFG_CP_SPC | DW_CFG_CP_SDC | DW_CFG_PDOA_MODE |
		      DW_CFG_FAST_AAT | DW_CFG_RXWTOE),
		    DW_CFG_DIS_DRXB | DW_CFG_CIA_IPATOV | DW_CFG_CIA_STS);
	g.sp3 = false;
	/* TX_FCTRL octets 4 and 5: FINE_PLEN 0, so TXPSR rules (8.2.2.8). */
	wr16(0x00, 0x28, 0u);

	/* STS length 64 (8.2.3.1), the CIA's STS tuning value (8.2.13.40). */
	wr8(DW_STS_CFG, 7u);
	wr8(DW_STS_CONF_1, 0x9Bu);
	g.key_valid = false;

	/* Antenna delays (8.2.2.19, 8.2.13.36). */
	wr16(DW_TX_ANTD, TU_TX_ANTD);
	wr_and_or32(DW_CIA_CONF, 0xFFFF0000u, TU_RX_ANTD);

	/* PLL: calibrate and lock (8.2.10.3), then let the sequencer run to IDLE_PLL
	 * (SEQ_CTRL.AINIT2IDLE, 8.2.15.3) and wait for CPLOCK. */
	status_clear(DW_ST_CPLOCK);
	wr16(DW_PLL_CAL, 0x0181u);
	wr_and_or32(DW_SEQ_CTRL, 0xFFFFFFFFu, 1u << 8);
	if (!wait_status(DW_ST_CPLOCK, 50u, 100u)) {
		return -EIO;
	}

	/* Receiver calibration (8.2.5.2 to 8.2.5.5), needed once before the first RX: COMP_DLY 2,
	 * calibration mode, CAL_EN, done flag, both results, normal mode. */
	/* RX (PGF) calibration with the PGF LDOs on: the API guide (5.2, dwt_pgf_cal) says they
	 * "should be enabled when running the function", the manual says only "see the API".
	 * Without them the calibration reports success and the receiver hears nothing. The bits
	 * are VDDMS1, VDDMS3 and VDDIF2 of LDO_CTRL (0x07:48). */
	ldo_ctrl = rd16(0x07, 0x48);
	wr16(0x07, 0x48, (uint16_t)(ldo_ctrl | 0x0105u));
	tu_bus_delay_us(20u);
	wr32(DW_RX_CAL, (2u << 16) | 1u);
	wr_and_or32(DW_RX_CAL, 0xFFFFFFFFu, 1u << 4);
	for (unsigned i = 0u; (rd8(DW_RX_CAL_STS) & 1u) == 0u; i++) {
		if (i >= 100u) {
			wr16(0x07, 0x48, ldo_ctrl);
			return -EIO;
		}
		tu_bus_delay_us(20u);
	}
	wr8(DW_RX_CAL, 0u);
	wr8(DW_RX_CAL_STS, 1u);
	cal_ok = rd32(DW_RX_CAL_RESI) != DW_RX_CAL_FAILED && rd32(DW_RX_CAL_RESQ) != DW_RX_CAL_FAILED;
	wr16(0x07, 0x48, ldo_ctrl);
	if (!cal_ok) {
		return -EIO;
	}

	/* Interrupts: the dispatch set, nothing from the high word, slate clean. */
	wr32(DW_SYS_ENABLE, DW_IRQ_MASK);
	wr16(0x00, 0x40, 0u);
	fast_cmd(DW_CMD_CLR_IRQS);
	return 0;
}

/* ── tinyuwb/hw.h ───────────────────────────────────────────────────────── */

void tu_hw_rx_regs(uint32_t out[14])
{
	out[0] = rd16(DW_DGC_CFG);
	out[1] = rd32(DW_DGC_CFG0);
	out[2] = rd32(DW_DGC_CFG1);
	for (uint8_t i = 0u; i < 7u; i++) {
		out[3u + i] = rd32(0x03, (uint8_t)(0x38u + 4u * i));
	}
	out[10] = rd32(DW_RF_SWITCH);
	out[11] = rd32(DW_SYS_CFG);
	out[12] = rd16(DW_DTUNE0);
	out[13] = rd16(DW_RX_SFD_TOC);
}

void tu_hw_debug(uint32_t out[5])
{
	out[0] = rd32(DW_SYS_STATUS);
	out[1] = rd32(DW_SYS_STATE);
	out[2] = rd32(DW_SYS_ENABLE);
	out[3] = rd16(DW_CHAN_CTRL);
	out[4] = (uint32_t)tu_bus_irq_level();
}

int tu_hw_listen_start(uint8_t channel, uint8_t code)
{
	int rc;

	if (!g.up) {
		rc = chip_init();
		if (rc != 0) {
			return rc;
		}
		g.up = true;
	}
	if (g.phy_valid && g.phy_channel == channel && g.phy_code == code) {
		fast_cmd(DW_CMD_TXRXOFF);
		tu_hw_sts_mode(false);
		return 0;
	}
	g.phy_valid = false;
	tu_bus_irq_enable(false);
	rc = phy_configure(channel, code);
	if (rc != 0) {
		return rc;
	}
	g.phy_valid = true;
	g.phy_channel = channel;
	g.phy_code = code;
	tu_bus_irq_enable(true);
	return 0;
}

void tu_hw_sleep(void)
{
	if (!g.up) {
		return;
	}
	tu_bus_irq_enable(false);
	fast_cmd(DW_CMD_TXRXOFF);
	/* DEEPSLEEP (8.2.11.6): sleep enabled, no sleep counter, wake on WAKEUP or SPICSn, then
	 * CFG_UPLOAD (8.2.11.2) takes the part down. The next listen resets and rebuilds it. */
	wr8(DW_AON_CFG, 0x19u);
	wr8(DW_AON_CTRL, 1u << 2);
	memset(&g, 0, sizeof(g));
}

int tu_hw_rx_enable(bool delayed, uint32_t at_hi32, uint32_t timeout)
{
	/* RX_FWTO is written in IDLE_PLL only (8.2.2.11); RXWTOE is SYS_CFG bit 9, octet 1
	 * bit 1. */
	if (timeout != 0u) {
		uint8_t b[3] = {(uint8_t)timeout, (uint8_t)(timeout >> 8),
				(uint8_t)((timeout >> 16) & 0x0Fu)};

		(void)reg_wr(DW_RX_FWTO, b, 3u);
		wr_and_or8(0x00, 0x11, 0xFFu, 0x02u);
	} else {
		wr_and_or8(0x00, 0x11, (uint8_t)~0x02u, 0u);
	}
	g.st.rx_arm++;
	if (!delayed) {
		fast_cmd(DW_CMD_RX);
		return 0;
	}
	wr32(DW_DX_TIME, at_hi32);
	fast_cmd(DW_CMD_DRX);
	/* HPDWARN (8.2.2.14): the start is more than half a wrap away, so it has passed. */
	if ((rd32(DW_SYS_STATUS) & DW_ST_HPDWARN) != 0u) {
		fast_cmd(DW_CMD_TXRXOFF);
		status_clear(DW_ST_HPDWARN);
		return -ETIME;
	}
	return 0;
}

void tu_hw_trx_off(void)
{
	fast_cmd(DW_CMD_TXRXOFF);
}

int tu_hw_tx_delayed(const uint8_t *data, uint16_t len, bool ranging, uint32_t at_hi32)
{
	if (len > 125u) {
		return -EINVAL;
	}
	if (len != 0u) {
		(void)reg_wr(DW_TX_BUFFER, 0u, data, len);
	}
	/* TXFLEN counts the FCS the chip appends (8.2.2.8); 6.8 Mbit/s; 64-symbol preamble. */
	wr32(DW_TX_FCTRL, (uint32_t)(len + 2u) | DW_TXF_TXBR_6M8 | (ranging ? DW_TXF_TR : 0u) |
				  DW_TXF_PSR_64);
	wr32(DW_DX_TIME, at_hi32);
	fast_cmd(DW_CMD_DTX);
	if ((rd32(DW_SYS_STATUS) & DW_ST_HPDWARN) != 0u) {
		fast_cmd(DW_CMD_TXRXOFF);
		status_clear(DW_ST_HPDWARN);
		return -ETIME;
	}
	return 0;
}

/* STS_KEY and STS_IV hold their 128 bits low octet first (8.2.3.4, 8.2.3.5); the engine's
 * dURSK and STS-V are in CCC order, most significant octet first. */
static void reverse16(uint8_t out[16], const uint8_t in[16])
{
	for (unsigned i = 0u; i < 16u; i++) {
		out[i] = in[15u - i];
	}
}

void tu_hw_sts_key(const uint8_t dursk[16])
{
	uint8_t img[16];

	reverse16(img, dursk);
	if (g.key_valid && memcmp(img, g.key, 16u) == 0) {
		return;
	}
	(void)reg_wr(DW_STS_KEY, img, 16u);
	memcpy(g.key, img, 16u);
	g.key_valid = true;
}

void tu_hw_sts_iv(const uint8_t sts_v[16])
{
	uint8_t img[16];

	reverse16(img, sts_v);
	(void)reg_wr(DW_STS_IV, img, 16u);
	wr8(DW_STS_CTRL, 1u); /* LOAD_IV */
}

void tu_hw_sts_mode(bool sp3)
{
	/* CP_SPC is SYS_CFG bits 13:12, octet 1 bits 5:4. The IV must be loaded after CP_SPC
	 * changes (8.2.2.4), so the latch is repeated when switching to SP3. */
	if (g.sp3 == sp3) {
		return;
	}
	wr_and_or8(0x00, 0x11, (uint8_t)~0x30u, sp3 ? 0x30u : 0u);
	if (sp3) {
		wr8(DW_STS_CTRL, 1u);
	}
	g.sp3 = sp3;
}

int tu_hw_sts_quality(int16_t *index)
{
	int16_t q = (int16_t)(rd16(DW_STS_STS) & 0x0FFFu);

	if ((q & 0x0800) != 0) {
		q = (int16_t)(q - 0x1000); /* 12-bit two's complement */
	}
	*index = q;
	return (int)q - TU_STS_QUAL_MIN;
}

void tu_hw_rx_read(uint8_t *buf, uint16_t len)
{
	(void)reg_rd(DW_RX_BUFFER_0, 0u, buf, len);
}

uint64_t tu_hw_rx_timestamp(void)
{
	return rd40(DW_IP_TS);
}

uint64_t tu_hw_tx_timestamp(void)
{
	return rd40(DW_TX_TIME);
}

uint32_t tu_hw_time_hi32(void)
{
	/* SYS_TIME latches on read; a write releases the latch (8.2.2.7). */
	wr8(DW_SYS_TIME, 0u);
	return rd32(DW_SYS_TIME);
}

/* ── Interrupt service (manual 2.3.2, 8.2.2.14) ──────────────────────────── */

void tu_dw3000_irq(void)
{
	uint32_t st;

	if (!g.up) {
		return;
	}
	st = rd32(DW_SYS_STATUS);
	g.st.irq++;
	g.st.last_status = st;
	if ((st & DW_ST_TXFRS) != 0u) {
		status_clear(DW_ST_TX_ALL);
		g.st.tx_done++;
		tu_round_tx_done();
	}
	if ((st & DW_ST_RX_ALL) == 0u) {
		return;
	}
	/* An SP3 frame has no PHR or payload, so RXFR alone is its good event (8.2.2.14, RXFR). */
	if ((st & DW_ST_RXFCG) != 0u || (g.sp3 && (st & DW_ST_RXFR) != 0u)) {
		uint32_t flags = 0u;
		uint16_t len = 0u;

		if ((st & DW_ST_RXFCG) != 0u) {
			flags |= TU_RX_FCG;
		}
		if ((st & DW_ST_RXPHD) != 0u) {
			flags |= TU_RX_PHD;
			len = (uint16_t)(rd16(DW_RX_FINFO) & 0x03FFu);
		}
		if ((st & DW_ST_CIADONE) != 0u) {
			flags |= TU_RX_CIA;
		}
		if ((st & DW_ST_CPERR) != 0u) {
			flags |= TU_RX_CPER;
		}
		status_clear(DW_ST_RX_ALL);
		g.st.rx_ok++;
		tu_round_rx_ok(flags, len);
	} else if ((st & DW_ST_RX_TO) != 0u) {
		status_clear(DW_ST_RX_ALL);
		g.st.rx_to++;
		g.st.e_pto += (st & DW_ST_RXPTO) != 0u;
		g.st.e_fto += (st & DW_ST_RXFTO) != 0u;
		tu_round_rx_ok(TU_RX_TO, 0u);
	} else if ((st & DW_ST_RX_ERR) != 0u) {
		uint32_t flags = TU_RX_ERR;
		uint16_t len = 0u;

		g.st.e_phe += (st & DW_ST_RXPHE) != 0u;
		g.st.e_fce += (st & DW_ST_RXFCE) != 0u;
		g.st.e_fsl += (st & DW_ST_RXFSL) != 0u;
		g.st.e_sto += (st & DW_ST_RXSTO) != 0u;
		g.st.e_arfe += (st & DW_ST_ARFE) != 0u;
		g.st.e_cia += (st & DW_ST_CIAERR) != 0u;
		if ((st & DW_ST_RXPHD) != 0u) {
			flags |= TU_RX_PHD;
			g.st.last_finfo = rd32(DW_RX_FINFO);
			len = (uint16_t)(g.st.last_finfo & 0x03FFu);
		}
		status_clear(DW_ST_RX_ALL);
		g.st.rx_err++;
		tu_round_rx_ok(flags, len);
	} else {
		status_clear(DW_ST_RX_ALL);
		g.st.rx_other++;
	}
}
