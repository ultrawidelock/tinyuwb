/** @file bus_zephyr.c — tinyuwb/bus.h on Zephyr: the SPI device and the three side-band
 * lines of a tinyuwb,dw3000 devicetree node, and the IRQ line into the system workqueue.
 *
 * The interrupt handler only queues work; every register access runs in the workqueue
 * thread, where the SPI driver may sleep. The work item re-reads the line after each
 * service pass so an event that landed during the pass is not lost (the chip holds IRQ
 * while any unmasked status bit is set, manual 2.3.2).
 */

#define DT_DRV_COMPAT tinyuwb_dw3000

#include "tinyuwb/bus.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>

static const struct spi_dt_spec spi =
	SPI_DT_SPEC_INST_GET(0, SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_MASTER);
static const struct gpio_dt_spec pin_reset = GPIO_DT_SPEC_INST_GET(0, reset_gpios);
static const struct gpio_dt_spec pin_irq = GPIO_DT_SPEC_INST_GET(0, irq_gpios);
static const struct gpio_dt_spec pin_wakeup = GPIO_DT_SPEC_INST_GET(0, wakeup_gpios);

static struct gpio_callback irq_cb;
static struct k_work irq_work;
static bool pins_ready;

static void irq_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	do {
		tu_dw3000_irq();
	} while (gpio_pin_get_dt(&pin_irq) > 0);
}

static void irq_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&irq_work);
}

static void pins_init(void)
{
	if (pins_ready) {
		return;
	}
	/* WAKEUP idles low; a reset on every wake makes the pin a spare. IRQ floats in
	 * DEEPSLEEP (manual 2.3.2), so it carries a pull-down. */
	(void)gpio_pin_configure_dt(&pin_wakeup, GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&pin_irq, GPIO_INPUT | GPIO_PULL_DOWN);
	k_work_init(&irq_work, irq_work_fn);
	gpio_init_callback(&irq_cb, irq_isr, BIT(pin_irq.pin));
	(void)gpio_add_callback(pin_irq.port, &irq_cb);
	pins_ready = true;
}

int tu_bus_write(const uint8_t *hdr, uint8_t hlen, const uint8_t *data, uint16_t len)
{
	const struct spi_buf tx[2] = {
		{.buf = (void *)hdr, .len = hlen},
		{.buf = (void *)data, .len = len},
	};
	const struct spi_buf_set tx_set = {.buffers = tx, .count = len != 0u ? 2u : 1u};

	return spi_write_dt(&spi, &tx_set);
}

int tu_bus_read(const uint8_t *hdr, uint8_t hlen, uint8_t *data, uint16_t len)
{
	const struct spi_buf tx[1] = {{.buf = (void *)hdr, .len = hlen}};
	const struct spi_buf rx[2] = {
		{.buf = NULL, .len = hlen}, /* the octets clocked in under the header */
		{.buf = data, .len = len},
	};
	const struct spi_buf_set tx_set = {.buffers = tx, .count = 1u};
	const struct spi_buf_set rx_set = {.buffers = rx, .count = 2u};

	return spi_transceive_dt(&spi, &tx_set, &rx_set);
}

void tu_bus_reset(void)
{
	pins_init();
	/* RSTn is driven low, never high: released it is an input (data sheet pin table, API
	 * guide 5.1.14). 10 ns is enough; 1 ms costs nothing. INIT_RC to IDLE_RC takes under
	 * 1 ms plus the 70 us OTP boot (manual figure 8); 3 ms covers it. */
	(void)gpio_pin_configure_dt(&pin_reset, GPIO_OUTPUT_ACTIVE);
	k_busy_wait(1000);
	(void)gpio_pin_configure_dt(&pin_reset, GPIO_INPUT);
	k_msleep(3);
}

void tu_bus_delay_us(uint32_t us)
{
	k_busy_wait(us);
}

void tu_bus_irq_enable(bool on)
{
	pins_init();
	(void)gpio_pin_interrupt_configure_dt(&pin_irq, on ? GPIO_INT_EDGE_TO_ACTIVE : GPIO_INT_DISABLE);
	if (on && gpio_pin_get_dt(&pin_irq) > 0) {
		k_work_submit(&irq_work); /* already asserted: no edge will come */
	}
}
