/** @file aes.h — the one block-cipher seam the engine needs. */

#ifndef TINYUWB_AES_H
#define TINYUWB_AES_H

#include <stddef.h>
#include <stdint.h>

/** @brief One AES block encrypt, 128- or 256-bit key (out may alias in).
 *
 * The port supplies it: tinypsa's backend on the target, a FIPS-197 reference on the host.
 * @return 0, -EINVAL on a null pointer or a key size other than 128 or 256 bits.
 */
int tu_aes_ecb(const uint8_t *key, size_t key_bits, const uint8_t in[16], uint8_t out[16]);

#endif /* TINYUWB_AES_H */
