/** @file dw3000_regs.h — the register subset the driver touches, as (file, offset) pairs
 * and bit positions, as in the DW3000 Family User Manual version 1.1 (2019), chapter 8,
 * and its fast-command list, chapter 9.
 *
 * Addressing: 32 register files of up to 128 directly addressable octets; a transaction
 * names a 5-bit file ID and a 7-bit octet offset (section 2.3.1.2). Multi-octet values are
 * little-endian: the octet at the lowest offset is the least significant.
 */

#ifndef TINYUWB_DW3000_REGS_H
#define TINYUWB_DW3000_REGS_H

/* Register file 0x00 (8.2.2): general configuration. */
#define DW_DEV_ID       0x00, 0x00 /* 4 B RO, 0xDECA0302 non-PDoA, 0xDECA0312 PDoA */
#define DW_SYS_CFG      0x00, 0x10 /* 4 B */
#define DW_SYS_TIME     0x00, 0x1C /* 4 B RO, high 32 of the 40-bit time, latched per read */
#define DW_TX_FCTRL     0x00, 0x24 /* 6 B */
#define DW_DX_TIME      0x00, 0x2C /* 4 B, high 32 of the 40-bit time, bit 0 ignored */
#define DW_RX_FWTO      0x00, 0x34 /* 3 B, 20-bit, units of 512/499.2 MHz */
#define DW_SYS_ENABLE   0x00, 0x3C /* 6 B */
#define DW_SYS_STATUS   0x00, 0x44 /* 6 B, write 1 to clear */
#define DW_SYS_STATE    0x0F, 0x30 /* 4 B RO: TX_STATE 3:0, RX_STATE 13:8, PMSC_STATE 23:16 */
#define DW_RX_FINFO     0x00, 0x4C /* 4 B RO */
#define DW_TX_TIME      0x00, 0x74 /* 5 B RO, antenna-adjusted 40-bit TX time */

/* Register file 0x01 (8.2.2.18 onwards). */
#define DW_TX_ANTD      0x01, 0x04 /* 2 B */
#define DW_TX_POWER     0x01, 0x0C /* 4 B */
#define DW_CHAN_CTRL    0x01, 0x14 /* 2 B */

/* Register file 0x02 (8.2.3): STS. */
#define DW_STS_CFG      0x02, 0x00 /* 2 B, CPS_LEN bits 7:0 in units of 8 */
#define DW_STS_CTRL     0x02, 0x04 /* 1 B, LOAD_IV bit 0 (self clearing) */
#define DW_STS_STS      0x02, 0x08 /* 2 B, ACC_QUAL bits 11:0 */
#define DW_STS_KEY      0x02, 0x0C /* 16 B, low octets at low offsets */
#define DW_STS_IV       0x02, 0x1C /* 16 B */

/* Register file 0x03 (8.2.4): receiver tuning. */
#define DW_DGC_CFG      0x03, 0x18 /* 2 B, RX_TUNE_EN bit 0, THR_64 bits 14:9 */
#define DW_DGC_CFG0     0x03, 0x1C /* 4 B */
#define DW_DGC_CFG1     0x03, 0x20 /* 4 B */
#define DW_DGC_LUT0     0x03, 0x38 /* 7 x 4 B, LUT_0..LUT_6 */
#define DW_RF_SWITCH    0x07, 0x14 /* 4 B */
#define DW_DGC_LUT0     0x03, 0x38 /* 7 x 4 B, LUT_n at 0x38 + 4 n */

/* Register file 0x04 (8.2.5): RX calibration. */
#define DW_RX_CAL       0x04, 0x0C /* 4 B, CAL_MODE bits 1:0, CAL_EN bit 4, COMP_DLY bits 19:16 */
#define DW_RX_CAL_RESI  0x04, 0x14 /* 4 B, 0x1FFFFFFF = failed */
#define DW_RX_CAL_RESQ  0x04, 0x1C /* 4 B */
#define DW_RX_CAL_STS   0x04, 0x20 /* 1 B, bit 0 done, write 1 to clear */

/* Register file 0x06 (8.2.7): digital receiver. */
#define DW_DTUNE0       0x06, 0x00 /* 2 B, PAC bits 1:0, DT0B4 bit 4 */
#define DW_RX_SFD_TOC   0x06, 0x02 /* 2 B, symbols */
#define DW_PRE_TOC      0x06, 0x04 /* 2 B, PAC units, 0 = off */
#define DW_DTUNE3       0x06, 0x0C /* 4 B */

/* Register file 0x07 (8.2.8): analog RF. */
#define DW_RF_TX_CTRL_1 0x07, 0x1A /* 1 B */
#define DW_RF_TX_CTRL_2 0x07, 0x1C /* 4 B, PG_DELAY bits 5:0 */
#define DW_LDO_RLOAD    0x07, 0x51 /* 1 B */

/* Register file 0x09 (8.2.10): frequency synthesiser. */
#define DW_PLL_CFG      0x09, 0x00 /* 2 B */
#define DW_PLL_CAL      0x09, 0x08 /* 2 B, USE_OLD bit 1, PLL_CFG_LD bits 7:2, CAL_EN bit 8 */
#define DW_XTAL         0x09, 0x14 /* 1 B, XTAL_TRIM */

/* Register file 0x0A (8.2.11): always-on. */
#define DW_AON_CTRL     0x0A, 0x04 /* 1 B, CFG_UPLOAD bit 2 */
#define DW_AON_CFG      0x0A, 0x14 /* 1 B, SLEEP_EN bit 0, WAKE_CSN bit 3, WAKE_WUP bit 4 */

/* Register file 0x0B (8.2.12): OTP interface. */
#define DW_OTP_ADDR     0x0B, 0x04 /* 2 B */
#define DW_OTP_CFG      0x0B, 0x08 /* 2 B, OTP_MAN bit 0, OTP_READ bit 1, DGC_KICK bit 6,
				      LDO_KICK bit 7, BIAS_KICK bit 8, DGC_SEL bit 13 */
#define DW_OTP_RDATA    0x0B, 0x10 /* 4 B RO */

/* Register files 0x0C and 0x0E (8.2.13): CIA. */
#define DW_IP_TS        0x0C, 0x00 /* 8 B RO, IP_TOA 40 bits then POA and status */
#define DW_CIA_CONF     0x0E, 0x00 /* 4 B, RXANTD bits 15:0 */
#define DW_STS_CONF_1   0x0E, 0x16 /* 4 B, RES_B0 bits 7:0 */

/* Register file 0x11 (8.2.15): PMSC. */
#define DW_SEQ_CTRL     0x11, 0x08 /* 4 B, AINIT2IDLE bit 8 */

/* Buffers. */
#define DW_RX_BUFFER_0  0x12
#define DW_TX_BUFFER    0x14

/* SYS_STATUS low word bits (8.2.2.14). */
#define DW_ST_CPLOCK    (1u << 1)
#define DW_ST_TXFRB     (1u << 4)
#define DW_ST_TXPRS     (1u << 5)
#define DW_ST_TXPHS     (1u << 6)
#define DW_ST_TXFRS     (1u << 7)
#define DW_ST_RXPRD     (1u << 8)
#define DW_ST_RXSFDD    (1u << 9)
#define DW_ST_CIADONE   (1u << 10)
#define DW_ST_RXPHD     (1u << 11)
#define DW_ST_RXPHE     (1u << 12)
#define DW_ST_RXFR      (1u << 13)
#define DW_ST_RXFCG     (1u << 14)
#define DW_ST_RXFCE     (1u << 15)
#define DW_ST_RXFSL     (1u << 16)
#define DW_ST_RXFTO     (1u << 17)
#define DW_ST_CIAERR    (1u << 18)
#define DW_ST_RXOVRR    (1u << 20)
#define DW_ST_RXPTO     (1u << 21)
#define DW_ST_SPIRDY    (1u << 23)
#define DW_ST_RCINIT    (1u << 24)
#define DW_ST_RXSTO     (1u << 26)
#define DW_ST_HPDWARN   (1u << 27)
#define DW_ST_CPERR     (1u << 28)
#define DW_ST_ARFE      (1u << 29)

#define DW_ST_TX_ALL    (DW_ST_TXFRB | DW_ST_TXPRS | DW_ST_TXPHS | DW_ST_TXFRS)
#define DW_ST_RX_GOOD   (DW_ST_RXPRD | DW_ST_RXSFDD | DW_ST_CIADONE | DW_ST_RXPHD | DW_ST_RXFR | \
			 DW_ST_RXFCG)
#define DW_ST_RX_TO     (DW_ST_RXFTO | DW_ST_RXPTO)
#define DW_ST_RX_ERR    (DW_ST_RXPHE | DW_ST_RXFCE | DW_ST_RXFSL | DW_ST_CIAERR | DW_ST_RXOVRR | \
			 DW_ST_RXSTO | DW_ST_ARFE)
#define DW_ST_RX_ALL    (DW_ST_RX_GOOD | DW_ST_RX_TO | DW_ST_RX_ERR | DW_ST_CPERR)

/* SYS_CFG bits (8.2.2.4). */
#define DW_CFG_FFEN       (1u << 0)
#define DW_CFG_DIS_DRXB   (1u << 3)
#define DW_CFG_PHR_MODE   (1u << 4)
#define DW_CFG_PHR_6M8    (1u << 5)
#define DW_CFG_CIA_IPATOV (1u << 7)
#define DW_CFG_CIA_STS    (1u << 8)
#define DW_CFG_RXWTOE     (1u << 9)
#define DW_CFG_RXAUTR     (1u << 10)
#define DW_CFG_AUTO_ACK   (1u << 11)
#define DW_CFG_CP_SPC     (3u << 12)
#define DW_CFG_CP_SDC     (1u << 15)
#define DW_CFG_PDOA_MODE  (3u << 16)
#define DW_CFG_FAST_AAT   (1u << 18)

/* TX_FCTRL low word (8.2.2.8). */
#define DW_TXF_TXBR_6M8   (1u << 10)
#define DW_TXF_TR         (1u << 11)
#define DW_TXF_PSR_64     (1u << 12)
#define DW_TXF_PSR_128    (5u << 12)

/* Fast commands (chapter 9). */
#define DW_CMD_TXRXOFF  0x00
#define DW_CMD_TX       0x01
#define DW_CMD_RX       0x02
#define DW_CMD_DTX      0x03
#define DW_CMD_DRX      0x04
#define DW_CMD_CLR_IRQS 0x12

/* OTP word addresses (7.3.1, table 17). */
#define DW_OTP_LDOTUNE_LO 0x04
#define DW_OTP_LDOTUNE_HI 0x05
#define DW_OTP_BIASTUNE   0x0A
#define DW_OTP_XTAL_TRIM  0x1E
#define DW_OTP_DGC_CFG0   0x20
#define DW_OTP_PLL_CODE   0x35

#endif /* TINYUWB_DW3000_REGS_H */
