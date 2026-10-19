/** @file bus_fake.h — a DW3000 on the host: decodes every SPI transaction into a register
 * image, logs it, and answers the few status bits the driver waits for. */
#ifndef TINYUWB_BUS_FAKE_H
#define TINYUWB_BUS_FAKE_H

#include <stdbool.h>
#include <stdint.h>

#define BF_FILES 32
#define BF_FILE_LEN 160
#define BF_LOG 512

struct bf_op {
	bool wr;
	uint8_t fast;   /* 1 when a fast command; the code is in file */
	uint8_t mode;   /* masked-write mode, 0 for plain */
	uint8_t file, off;
	uint16_t len;
	uint8_t data[16]; /* first bytes written */
};

struct bus_fake {
	uint8_t img[BF_FILES][BF_FILE_LEN];
	uint32_t otp[128];
	struct bf_op log[BF_LOG];
	unsigned nlog, dropped;
	unsigned resets, cmds[32];
	unsigned iv_loads;
	unsigned irq_on, irq_off;
	bool hpdwarn;      /* set HPDWARN when the next delayed command lands */
	bool rx_cal_fail;  /* report 0x1FFFFFFF from RX_CAL_RESI */
	bool no_cplock;    /* never set CPLOCK */
	uint32_t devid;    /* what DEV_ID reads after reset */
	uint32_t sys_time; /* what SYS_TIME reads, advanced per write */
};

extern struct bus_fake bf;

void bf_reset(void);
/** The last value written to (file, off) as a little-endian word, or 0xFFFFFFFF when none. */
uint32_t bf_last_write(uint8_t file, uint8_t off);
/** How many plain writes hit (file, off). */
unsigned bf_writes(uint8_t file, uint8_t off);
/** The last masked write at (file, off): returns the mode, fills and/or. 0 when none. */
uint8_t bf_last_masked(uint8_t file, uint8_t off, uint32_t *and_mask, uint32_t *or_mask);
/** Reads of (file, off). */
unsigned bf_reads(uint8_t file, uint8_t off);
/** Register image word, little-endian. */
uint32_t bf_reg32(uint8_t file, uint8_t off);
void bf_set32(uint8_t file, uint8_t off, uint32_t v);

#endif /* TINYUWB_BUS_FAKE_H */
