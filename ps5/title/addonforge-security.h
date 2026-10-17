#ifndef PS5_TITLE_ADDONFORGE_SECURITY_H
#define PS5_TITLE_ADDONFORGE_SECURITY_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*forever_entropy_fill)(uint8_t *, size_t);


static inline int forever_entropy_hex(char *output, size_t output_size,
                                      size_t random_bytes, forever_entropy_fill fill)
{
    static const char digits[] = "0123456789abcdef";
    uint8_t bytes[64];
    if (!output || !fill || !random_bytes || random_bytes > sizeof(bytes) ||
        output_size != random_bytes * 2 + 1) return -EINVAL;
    if (fill(bytes, random_bytes)) return -EIO;
    for (size_t i = 0; i < random_bytes; i++)
    {
        output[2 * i] = digits[bytes[i] >> 4];
        output[2 * i + 1] = digits[bytes[i] & 15];
    }
    output[2 * random_bytes] = 0;
    return 0;
}

#endif
