/** @file port_zephyr.c — tinyuwb/aes.h and tinyuwb/port.h on Zephyr: the block cipher from
 * tinypsa's backend, the clock from the kernel. */

#include "tinyuwb/aes.h"
#include "tinyuwb/port.h"

#include <errno.h>
#include <string.h>

#include <tinypsa/backend.h>
#include <tinypsa_backend_ctx.h>
#include <zephyr/kernel.h>

int tu_aes_ecb(const uint8_t *key, size_t key_bits, const uint8_t in[16], uint8_t out[16])
{
	uint8_t ctx[TINYPSA_AES_CTX_SIZE];

	if (key == NULL || in == NULL || out == NULL || (key_bits != 128u && key_bits != 256u)) {
		return -EINVAL;
	}
	tp_be_aes_init(ctx, key, key_bits / 8u);
	tp_be_aes_encrypt(ctx, out, in, 16u);
	tp_be_ctx_free(ctx, sizeof(ctx));
	memset(ctx, 0, sizeof(ctx));
	return 0;
}

int64_t tu_uptime_ms(void)
{
	return k_uptime_get();
}
