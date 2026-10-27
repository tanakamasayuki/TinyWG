// BLAKE2s, written from RFC 7693.
#include "blake2s.h"

#include <string.h>

static const uint32_t blake2s_iv[8] = {0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
                                       0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19};

static const uint8_t blake2s_sigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}, {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4}, {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13}, {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11}, {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5}, {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static uint32_t load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define G(a, b, c, d, x, y)                                                                                            \
    do {                                                                                                               \
        v[a] = v[a] + v[b] + (x);                                                                                      \
        v[d] = rotr32(v[d] ^ v[a], 16);                                                                                \
        v[c] = v[c] + v[d];                                                                                            \
        v[b] = rotr32(v[b] ^ v[c], 12);                                                                                \
        v[a] = v[a] + v[b] + (y);                                                                                      \
        v[d] = rotr32(v[d] ^ v[a], 8);                                                                                 \
        v[c] = v[c] + v[d];                                                                                            \
        v[b] = rotr32(v[b] ^ v[c], 7);                                                                                 \
    } while (0)

static void blake2s_compress(tinywg_blake2s_ctx *ctx, int last)
{
    uint32_t v[16], m[16];
    for (int i = 0; i < 8; i++) {
        v[i] = ctx->h[i];
        v[i + 8] = blake2s_iv[i];
    }
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (last) v[14] = ~v[14];
    for (int i = 0; i < 16; i++) m[i] = load32_le(ctx->buf + 4 * i);
    for (int r = 0; r < 10; r++) {
        const uint8_t *s = blake2s_sigma[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) ctx->h[i] ^= v[i] ^ v[i + 8];
}

static void blake2s_increment(tinywg_blake2s_ctx *ctx, uint32_t n)
{
    ctx->t[0] += n;
    if (ctx->t[0] < n) ctx->t[1]++;
}

void tinywg_blake2s_init(tinywg_blake2s_ctx *ctx, size_t outlen, const uint8_t *key, size_t keylen)
{
    for (int i = 0; i < 8; i++) ctx->h[i] = blake2s_iv[i];
    ctx->h[0] ^= 0x01010000u ^ ((uint32_t)keylen << 8) ^ (uint32_t)outlen;
    ctx->t[0] = 0;
    ctx->t[1] = 0;
    ctx->buflen = 0;
    ctx->outlen = outlen;
    memset(ctx->buf, 0, sizeof(ctx->buf));
    if (keylen > 0) {
        memcpy(ctx->buf, key, keylen);
        ctx->buflen = 64;   // the key block is compressed as a full block
    }
}

void tinywg_blake2s_update(tinywg_blake2s_ctx *ctx, const uint8_t *in, size_t inlen)
{
    while (inlen > 0) {
        // The last block must be kept for final(), so compress only when more input follows.
        if (ctx->buflen == 64) {
            blake2s_increment(ctx, 64);
            blake2s_compress(ctx, 0);
            ctx->buflen = 0;
        }
        size_t n = 64 - ctx->buflen;
        if (n > inlen) n = inlen;
        memcpy(ctx->buf + ctx->buflen, in, n);
        ctx->buflen += n;
        in += n;
        inlen -= n;
    }
}

void tinywg_blake2s_final(tinywg_blake2s_ctx *ctx, uint8_t *out)
{
    blake2s_increment(ctx, (uint32_t)ctx->buflen);
    memset(ctx->buf + ctx->buflen, 0, 64 - ctx->buflen);
    blake2s_compress(ctx, 1);
    uint8_t full[32];
    for (int i = 0; i < 8; i++) {
        full[4 * i + 0] = (uint8_t)(ctx->h[i]);
        full[4 * i + 1] = (uint8_t)(ctx->h[i] >> 8);
        full[4 * i + 2] = (uint8_t)(ctx->h[i] >> 16);
        full[4 * i + 3] = (uint8_t)(ctx->h[i] >> 24);
    }
    memcpy(out, full, ctx->outlen);
    volatile uint8_t *p = (volatile uint8_t *)ctx;
    for (size_t i = 0; i < sizeof(*ctx); i++) p[i] = 0;
    p = full;
    for (size_t i = 0; i < sizeof(full); i++) p[i] = 0;
}

void tinywg_blake2s(uint8_t *out, size_t outlen, const uint8_t *key, size_t keylen, const uint8_t *in,
                    size_t inlen)
{
    tinywg_blake2s_ctx ctx;
    tinywg_blake2s_init(&ctx, outlen, key, keylen);
    tinywg_blake2s_update(&ctx, in, inlen);
    tinywg_blake2s_final(&ctx, out);
}
