/* Allocation-free streaming SHA-256 shared by ELF verification and prefix sync. */
#ifndef PW_SHA256_H
#define PW_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct sha256_context {
    uint32_t state[8];
    uint64_t length;
    unsigned char block[64];
    size_t used;
};
static inline uint32_t rotate_right(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32 - shift));
}
static inline uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static inline void store_be32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24); p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8); p[3] = (unsigned char)value;
}
static inline void sha256_transform(struct sha256_context *context, const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t words[64];
    for (unsigned i = 0; i < 16; ++i) words[i] = load_be32(block + i * 4);
    for (unsigned i = 16; i < 64; ++i) {
        uint32_t x = words[i - 15], y = words[i - 2];
        uint32_t a = rotate_right(x, 7) ^ rotate_right(x, 18) ^ (x >> 3);
        uint32_t b = rotate_right(y, 17) ^ rotate_right(y, 19) ^ (y >> 10);
        words[i] = words[i - 16] + a + words[i - 7] + b;
    }
    uint32_t a = context->state[0], b = context->state[1], c = context->state[2];
    uint32_t d = context->state[3], e = context->state[4], f = context->state[5];
    uint32_t g = context->state[6], h = context->state[7];
    for (unsigned i = 0; i < 64; ++i) {
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t temporary1 = h + sum1 + choice + constants[i] + words[i];
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temporary2 = sum0 + majority;
        h = g; g = f; f = e; e = d + temporary1;
        d = c; c = b; b = a; a = temporary1 + temporary2;
    }
    context->state[0] += a; context->state[1] += b;
    context->state[2] += c; context->state[3] += d;
    context->state[4] += e; context->state[5] += f;
    context->state[6] += g; context->state[7] += h;
}
static inline void sha256_init(struct sha256_context *context)
{
    static const uint32_t initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memcpy(context->state, initial, sizeof(initial));
    context->length = 0; context->used = 0;
}
static inline void sha256_update(struct sha256_context *context, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    context->length += (uint64_t)size;
    while (size) {
        size_t available = sizeof(context->block) - context->used;
        size_t take = size < available ? size : available;
        memcpy(context->block + context->used, bytes, take);
        context->used += take; bytes += take; size -= take;
        if (context->used == sizeof(context->block)) {
            sha256_transform(context, context->block);
            context->used = 0;
        }
    }
}
static inline void sha256_final(struct sha256_context *context, unsigned char digest[32])
{
    uint64_t bits = context->length << 3;
    unsigned char padding[64] = {0x80};
    size_t padding_size = context->used < 56 ? 56 - context->used : 120 - context->used;
    sha256_update(context, padding, padding_size);
    unsigned char length[8];
    for (unsigned i = 0; i < 8; ++i) length[7 - i] = (unsigned char)(bits >> (i * 8));
    sha256_update(context, length, sizeof(length));
    for (unsigned i = 0; i < 8; ++i) store_be32(digest + i * 4, context->state[i]);
}
#endif
