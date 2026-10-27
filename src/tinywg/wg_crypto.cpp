#include "wg_crypto.h"

#include <string.h>

#include "../third_party/monocypher/tinywg_monocypher.h"
#include "blake2s.h"

namespace tinywg {
namespace crypto {

static const uint8_t kZero32[32] = {0};

void hash(uint8_t out[kHashSize], const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    tinywg_blake2s_ctx ctx;
    tinywg_blake2s_init(&ctx, kHashSize, nullptr, 0);
    if (alen) tinywg_blake2s_update(&ctx, a, alen);
    if (blen) tinywg_blake2s_update(&ctx, b, blen);
    tinywg_blake2s_final(&ctx, out);
}

void mac(uint8_t out[kMacSize], const uint8_t *key, size_t keylen, const uint8_t *in, size_t len)
{
    tinywg_blake2s(out, kMacSize, key, keylen, in, len);
}

// HMAC-BLAKE2s-256 (RFC 2104, block size 64). Keys here are never longer than a block.
static void hmac(uint8_t out[kHashSize], const uint8_t *key, size_t keylen, const uint8_t *in1, size_t len1,
                 const uint8_t *in2, size_t len2)
{
    uint8_t pad[64];
    uint8_t inner[kHashSize];
    tinywg_blake2s_ctx ctx;

    memset(pad, 0x36, sizeof(pad));
    for (size_t i = 0; i < keylen; i++) pad[i] ^= key[i];
    tinywg_blake2s_init(&ctx, kHashSize, nullptr, 0);
    tinywg_blake2s_update(&ctx, pad, sizeof(pad));
    if (len1) tinywg_blake2s_update(&ctx, in1, len1);
    if (len2) tinywg_blake2s_update(&ctx, in2, len2);
    tinywg_blake2s_final(&ctx, inner);

    memset(pad, 0x5c, sizeof(pad));
    for (size_t i = 0; i < keylen; i++) pad[i] ^= key[i];
    tinywg_blake2s_init(&ctx, kHashSize, nullptr, 0);
    tinywg_blake2s_update(&ctx, pad, sizeof(pad));
    tinywg_blake2s_update(&ctx, inner, sizeof(inner));
    tinywg_blake2s_final(&ctx, out);

    wipe(pad, sizeof(pad));
    wipe(inner, sizeof(inner));
}

void kdf(uint8_t *t1, uint8_t *t2, uint8_t *t3, const uint8_t key[kKeySize], const uint8_t *in, size_t len)
{
    uint8_t prk[kHashSize];
    uint8_t t[kHashSize];
    uint8_t n;

    hmac(prk, key, kKeySize, in, len, nullptr, 0);
    n = 1;
    hmac(t, prk, sizeof(prk), &n, 1, nullptr, 0);
    memcpy(t1, t, kHashSize);
    if (t2) {
        n = 2;
        hmac(t, prk, sizeof(prk), t, kHashSize, &n, 1);
        memcpy(t2, t, kHashSize);
    }
    if (t3) {
        n = 3;
        hmac(t, prk, sizeof(prk), t, kHashSize, &n, 1);
        memcpy(t3, t, kHashSize);
    }
    wipe(prk, sizeof(prk));
    wipe(t, sizeof(t));
}

static void ietfNonce(uint8_t nonce[12], uint64_t counter)
{
    memset(nonce, 0, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)(counter >> (8 * i));
}

void aeadEncrypt(uint8_t *out, const uint8_t key[kKeySize], uint64_t counter, const uint8_t *in, size_t len,
                 const uint8_t *ad, size_t adlen)
{
    uint8_t nonce[12];
    tinywg_mc_aead_ctx ctx;
    ietfNonce(nonce, counter);
    tinywg_mc_aead_init_ietf(&ctx, key, nonce);
    tinywg_mc_aead_write(&ctx, out, out + len, ad, adlen, in, len);
    wipe(&ctx, sizeof(ctx));
}

bool aeadDecrypt(uint8_t *out, const uint8_t key[kKeySize], uint64_t counter, const uint8_t *in, size_t len,
                 const uint8_t *ad, size_t adlen)
{
    uint8_t nonce[12];
    tinywg_mc_aead_ctx ctx;
    ietfNonce(nonce, counter);
    tinywg_mc_aead_init_ietf(&ctx, key, nonce);
    int mismatch = tinywg_mc_aead_read(&ctx, out, in + len, ad, adlen, in, len);
    wipe(&ctx, sizeof(ctx));
    return mismatch == 0;
}

void xaeadEncrypt(uint8_t *out, const uint8_t key[kKeySize], const uint8_t nonce[24], const uint8_t *in, size_t len,
                  const uint8_t *ad, size_t adlen)
{
    tinywg_mc_aead_lock(out, out + len, key, nonce, ad, adlen, in, len);
}

bool xaeadDecrypt(uint8_t *out, const uint8_t key[kKeySize], const uint8_t nonce[24], const uint8_t *in, size_t len,
                  const uint8_t *ad, size_t adlen)
{
    return tinywg_mc_aead_unlock(out, in + len, key, nonce, ad, adlen, in, len) == 0;
}

void publicKey(uint8_t pub[kKeySize], const uint8_t priv[kKeySize]) { tinywg_mc_x25519_public_key(pub, priv); }

bool dh(uint8_t out[kKeySize], const uint8_t priv[kKeySize], const uint8_t pub[kKeySize])
{
    tinywg_mc_x25519(out, priv, pub);
    return !isZero32(out);
}

void clampPrivateKey(uint8_t key[kKeySize])
{
    key[0] &= 248;
    key[31] &= 127;
    key[31] |= 64;
}

bool equal16(const uint8_t *a, const uint8_t *b) { return tinywg_mc_verify16(a, b) == 0; }
bool equal32(const uint8_t *a, const uint8_t *b) { return tinywg_mc_verify32(a, b) == 0; }
bool isZero32(const uint8_t *a) { return tinywg_mc_verify32(a, kZero32) == 0; }
void wipe(void *p, size_t len) { tinywg_mc_wipe(p, len); }

}  // namespace crypto
}  // namespace tinywg
