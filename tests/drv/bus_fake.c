/** @file bus_fake.c — a DW3000 on the host, see bus_fake.h. */
#include "bus_fake.h"

#include <string.h>

#include "tinyuwb/bus.h"

struct bus_fake bf;

/* Register positions the fake reacts to; kept apart from the driver's header on purpose so a
 * driver typo cannot cancel out in the test. */
#define F_GEN 0x00
#define O_DEV_ID 0x00
#define O_SYS_TIME 0x1C
#define O_SYS_STATUS 0x44
#define F_STS 0x02
#define O_STS_CTRL 0x04
#define F_RXCAL 0x04
#define O_RX_CAL 0x0C
#define O_RX_CAL_RESI 0x14
#define O_RX_CAL_RESQ 0x1C
#define O_RX_CAL_STS 0x20
#define F_OTP 0x0B
#define O_OTP_ADDR 0x04
#define O_OTP_CFG 0x08
#define O_OTP_RDATA 0x10
#define F_PMSC 0x11
#define O_SEQ_CTRL 0x08

#define ST_CPLOCK (1u << 1)
#define ST_SPIRDY (1u << 23)
#define ST_RCINIT (1u << 24)
#define ST_HPDWARN (1u << 27)

static uint32_t get32(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
}

static void set32(uint8_t *b, uint32_t v)
{
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

uint32_t bf_reg32(uint8_t file, uint8_t off)
{
	return get32(&bf.img[file][off]);
}

void bf_set32(uint8_t file, uint8_t off, uint32_t v)
{
	set32(&bf.img[file][off], v);
}

void bf_reset(void)
{
	memset(&bf, 0, sizeof(bf));
	bf.devid = 0xDECA0302u;
}

static struct bf_op *log_new(void)
{
	if (bf.nlog >= BF_LOG) {
		bf.dropped++;
		return NULL;
	}
	return &bf.log[bf.nlog++];
}

/* The chip's side effects of a write. */
static void after_write(uint8_t file, uint8_t off, uint16_t len)
{
	if (file == F_OTP && off == O_OTP_CFG && (bf.img[F_OTP][O_OTP_CFG] & 0x02u) != 0u) {
		uint16_t a = (uint16_t)(bf.img[F_OTP][O_OTP_ADDR] |
					(bf.img[F_OTP][O_OTP_ADDR + 1] << 8));

		set32(&bf.img[F_OTP][O_OTP_RDATA], bf.otp[a & 0x7Fu]);
	}
	if (file == F_PMSC && off <= O_SEQ_CTRL && off + len > O_SEQ_CTRL + 1 &&
	    (bf.img[F_PMSC][O_SEQ_CTRL + 1] & 0x01u) != 0u && !bf.no_cplock) {
		bf.img[F_GEN][O_SYS_STATUS] |= (uint8_t)ST_CPLOCK;
	}
	if (file == F_RXCAL && off == O_RX_CAL && (bf.img[F_RXCAL][O_RX_CAL] & 0x10u) != 0u) {
		bf.img[F_RXCAL][O_RX_CAL] &= (uint8_t)~0x10u; /* CAL_EN self-clears */
		bf.img[F_RXCAL][O_RX_CAL_STS] = 1u;
		set32(&bf.img[F_RXCAL][O_RX_CAL_RESI], bf.rx_cal_fail ? 0x1FFFFFFFu : 0x00123456u);
		set32(&bf.img[F_RXCAL][O_RX_CAL_RESQ], 0x00654321u);
	}
	if (file == F_STS && off == O_STS_CTRL && (bf.img[F_STS][O_STS_CTRL] & 0x01u) != 0u) {
		bf.iv_loads++;
		bf.img[F_STS][O_STS_CTRL] = 0u; /* self clearing */
	}
	if (file == F_GEN && off == O_SYS_TIME) {
		bf.sys_time += 0x1000u;
		set32(&bf.img[F_GEN][O_SYS_TIME], bf.sys_time);
	}
}

static void fast_command(uint8_t cmd)
{
	bf.cmds[cmd & 0x1Fu]++;
	if ((cmd == 0x03u || cmd == 0x04u) && bf.hpdwarn) {
		bf.img[F_GEN][O_SYS_STATUS + 3] |= (uint8_t)(ST_HPDWARN >> 24);
		bf.hpdwarn = false;
	}
	if (cmd == 0x12u) {
		memset(&bf.img[F_GEN][O_SYS_STATUS], 0, 6u);
	}
}

/* Decode a header: returns 1 for fast command, 0 otherwise. */
static int decode(const uint8_t *hdr, uint8_t hlen, bool *wr, uint8_t *file, uint8_t *off,
		  uint8_t *mode)
{
	*wr = (hdr[0] & 0x80u) != 0u;
	*file = (uint8_t)((hdr[0] >> 1) & 0x1Fu);
	*mode = 0u;
	*off = 0u;
	if ((hdr[0] & 0x40u) == 0u) {
		if ((hdr[0] & 0x01u) != 0u) {
			return 1; /* fast command: bit 7 and bit 0 set, no second octet */
		}
		return 0;
	}
	if (hlen < 2u) {
		return -1;
	}
	*off = (uint8_t)(((hdr[0] & 0x01u) << 6) | (hdr[1] >> 2));
	*mode = (uint8_t)(hdr[1] & 0x03u);
	return 0;
}

int tu_bus_write(const uint8_t *hdr, uint8_t hlen, const uint8_t *data, uint16_t len)
{
	bool wr;
	uint8_t file, off, mode;
	int fast = decode(hdr, hlen, &wr, &file, &off, &mode);
	struct bf_op *op = log_new();

	if (fast < 0 || !wr) {
		return -1;
	}
	if (op != NULL) {
		op->wr = true;
		op->fast = (uint8_t)fast;
		op->mode = mode;
		op->file = file;
		op->off = off;
		op->len = len;
		memcpy(op->data, data, len < sizeof(op->data) ? len : sizeof(op->data));
	}
	if (fast == 1) {
		fast_command(file);
		return 0;
	}
	if (off + len > BF_FILE_LEN) {
		return -1;
	}
	if (mode == 0u) {
		if (file == F_GEN && off == O_SYS_STATUS) {
			for (uint16_t i = 0; i < len && i < 6u; i++) {
				bf.img[file][off + i] &= (uint8_t)~data[i]; /* write 1 to clear */
			}
		} else {
			memcpy(&bf.img[file][off], data, len);
		}
	} else {
		uint16_t n = (uint16_t)(1u << (mode - 1u));

		if (len != 2u * n) {
			return -1;
		}
		for (uint16_t i = 0; i < n; i++) {
			bf.img[file][off + i] = (uint8_t)((bf.img[file][off + i] & data[i]) | data[n + i]);
		}
		len = n;
	}
	after_write(file, off, len);
	return 0;
}

int tu_bus_read(const uint8_t *hdr, uint8_t hlen, uint8_t *data, uint16_t len)
{
	bool wr;
	uint8_t file, off, mode;
	int fast = decode(hdr, hlen, &wr, &file, &off, &mode);
	struct bf_op *op = log_new();

	if (fast != 0 || wr || mode != 0u || off + len > BF_FILE_LEN) {
		return -1;
	}
	if (op != NULL) {
		op->wr = false;
		op->file = file;
		op->off = off;
		op->len = len;
	}
	memcpy(data, &bf.img[file][off], len);
	return 0;
}

void tu_bus_reset(void)
{
	bf.resets++;
	memset(bf.img, 0, sizeof(bf.img));
	set32(&bf.img[F_GEN][O_DEV_ID], bf.devid);
	bf.img[F_GEN][O_SYS_STATUS + 2] |= (uint8_t)(ST_SPIRDY >> 16);
	bf.img[F_GEN][O_SYS_STATUS + 3] |= (uint8_t)(ST_RCINIT >> 24);
}

void tu_bus_delay_us(uint32_t us)
{
	(void)us;
}

void tu_bus_irq_enable(bool on)
{
	if (on) {
		bf.irq_on++;
	} else {
		bf.irq_off++;
	}
}

uint32_t bf_last_write(uint8_t file, uint8_t off)
{
	for (unsigned i = bf.nlog; i-- > 0u;) {
		const struct bf_op *op = &bf.log[i];

		if (op->wr && !op->fast && op->mode == 0u && op->file == file && op->off == off) {
			uint8_t b[4] = {0};

			memcpy(b, op->data, op->len < 4u ? op->len : 4u);
			return get32(b);
		}
	}
	return 0xFFFFFFFFu;
}

unsigned bf_writes(uint8_t file, uint8_t off)
{
	unsigned n = 0u;

	for (unsigned i = 0u; i < bf.nlog; i++) {
		const struct bf_op *op = &bf.log[i];

		if (op->wr && !op->fast && op->mode == 0u && op->file == file && op->off == off) {
			n++;
		}
	}
	return n;
}

uint8_t bf_last_masked(uint8_t file, uint8_t off, uint32_t *and_mask, uint32_t *or_mask)
{
	for (unsigned i = bf.nlog; i-- > 0u;) {
		const struct bf_op *op = &bf.log[i];

		if (op->wr && !op->fast && op->mode != 0u && op->file == file && op->off == off) {
			uint8_t n = (uint8_t)(1u << (op->mode - 1u));
			uint8_t a[4] = {0}, o[4] = {0};

			memcpy(a, op->data, n);
			memcpy(o, op->data + n, n);
			*and_mask = get32(a);
			*or_mask = get32(o);
			return op->mode;
		}
	}
	return 0u;
}

unsigned bf_reads(uint8_t file, uint8_t off)
{
	unsigned n = 0u;

	for (unsigned i = 0u; i < bf.nlog; i++) {
		if (!bf.log[i].wr && bf.log[i].file == file && bf.log[i].off == off) {
			n++;
		}
	}
	return n;
}
