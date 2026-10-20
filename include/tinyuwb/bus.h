/* SPDX-FileCopyrightText: 2026 Aseem Chopra */
/* SPDX-License-Identifier: Apache-2.0 */

/** @file bus.h — what the DW3000 register driver asks of the platform: raw SPI transfers,
 * the reset line, a microsecond delay and the interrupt line.
 *
 * Implemented by port/zephyr/bus_zephyr.c on the target and by tests/drv/bus_fake.c on the
 * host. A transfer is one chip-select envelope: the header bytes go out first, then either
 * the data bytes go out (write) or the same number of bytes come back (read).
 */

#ifndef TINYUWB_BUS_H
#define TINYUWB_BUS_H

#include <stdbool.h>
#include <stdint.h>

/** @brief One SPI write: @p hdr (@p hlen bytes) then @p data (@p len bytes), one envelope. */
int tu_bus_write(const uint8_t *hdr, uint8_t hlen, const uint8_t *data, uint16_t len);

/** @brief One SPI read: @p hdr (@p hlen bytes) out, then @p len bytes into @p data. */
int tu_bus_read(const uint8_t *hdr, uint8_t hlen, uint8_t *data, uint16_t len);

/** @brief Pulse RSTn low, release it (open drain, never driven high) and wait for the part
 * to reach IDLE_RC: at least 2 ms after the release. */
void tu_bus_reset(void);

/** @brief Busy-wait @p us microseconds. */
void tu_bus_delay_us(uint32_t us);

/** @brief Enable or disable the IRQ line's interrupt. While disabled the line is ignored. */
void tu_bus_irq_enable(bool on);

/** The IRQ line as the pin reads now: 1 asserted, 0 idle. */
int tu_bus_irq_level(void);

/* ── Upward: what the port calls ────────────────────────────────────────── */

/** @brief Service the chip's IRQ: read and clear the events, hand them to the engine. The port
 * calls it from thread context, repeating while the IRQ line stays asserted. */
void tu_dw3000_irq(void);

#endif /* TINYUWB_BUS_H */
