/** @file port.h — what the engine asks of the platform, beyond tinyuwb/aes.h. */

#ifndef TINYUWB_PORT_H
#define TINYUWB_PORT_H

#include <stdint.h>

/** @brief Milliseconds since boot, monotonic. */
int64_t tu_uptime_ms(void);

#endif /* TINYUWB_PORT_H */
