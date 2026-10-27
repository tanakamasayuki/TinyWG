// BLAKE2s (RFC 7693), keyed and unkeyed, digest length 1..32.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t h[8];
    uint32_t t[2];
    uint8_t buf[64];
    size_t buflen;
    size_t outlen;
} tinywg_blake2s_ctx;

void tinywg_blake2s_init(tinywg_blake2s_ctx *ctx, size_t outlen, const uint8_t *key, size_t keylen);
void tinywg_blake2s_update(tinywg_blake2s_ctx *ctx, const uint8_t *in, size_t inlen);
void tinywg_blake2s_final(tinywg_blake2s_ctx *ctx, uint8_t *out);

void tinywg_blake2s(uint8_t *out, size_t outlen, const uint8_t *key, size_t keylen, const uint8_t *in,
                    size_t inlen);

#ifdef __cplusplus
}
#endif
