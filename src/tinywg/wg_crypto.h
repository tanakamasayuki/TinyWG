// The primitives the WireGuard handshake is written in (whitepaper §5.4):
// HASH, MAC, HMAC, KDFn, AEAD, XAEAD and DH.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tinywg {
namespace crypto {

constexpr size_t kKeySize = 32;
constexpr size_t kHashSize = 32;
constexpr size_t kMacSize = 16;
constexpr size_t kTagSize = 16;

// HASH(a || b || c): BLAKE2s-256. Null / zero-length parts are skipped.
void hash(uint8_t out[kHashSize], const uint8_t *a, size_t alen, const uint8_t *b = nullptr, size_t blen = 0);

// MAC(key, in): keyed BLAKE2s-128. The key is 32 bytes, or 16 for a cookie.
void mac(uint8_t out[kMacSize], const uint8_t *key, size_t keylen, const uint8_t *in, size_t len);

// KDFn(key, input), n = 1..3. Outputs past the first may be null.
void kdf(uint8_t *t1, uint8_t *t2, uint8_t *t3, const uint8_t key[kKeySize], const uint8_t *in, size_t len);

// ChaCha20-Poly1305 (RFC 8439), nonce = 32 zero bits || 64-bit little-endian counter.
// out holds len + kTagSize bytes and may be the same buffer as in.
void aeadEncrypt(uint8_t *out, const uint8_t key[kKeySize], uint64_t counter, const uint8_t *in, size_t len,
                 const uint8_t *ad, size_t adlen);
// in holds len + kTagSize bytes; out gets len bytes and may be the same buffer as in.
bool aeadDecrypt(uint8_t *out, const uint8_t key[kKeySize], uint64_t counter, const uint8_t *in, size_t len,
                 const uint8_t *ad, size_t adlen);

// XChaCha20-Poly1305, for the cookie reply.
void xaeadEncrypt(uint8_t *out, const uint8_t key[kKeySize], const uint8_t nonce[24], const uint8_t *in, size_t len,
                  const uint8_t *ad, size_t adlen);
bool xaeadDecrypt(uint8_t *out, const uint8_t key[kKeySize], const uint8_t nonce[24], const uint8_t *in, size_t len,
                  const uint8_t *ad, size_t adlen);

// X25519. dh() is false when the result is all zero (a low-order public key).
void publicKey(uint8_t pub[kKeySize], const uint8_t priv[kKeySize]);
bool dh(uint8_t out[kKeySize], const uint8_t priv[kKeySize], const uint8_t pub[kKeySize]);
// Clamps a random 32 bytes into an X25519 private key.
void clampPrivateKey(uint8_t key[kKeySize]);

bool equal16(const uint8_t *a, const uint8_t *b);
bool equal32(const uint8_t *a, const uint8_t *b);
bool isZero32(const uint8_t *a);
void wipe(void *p, size_t len);

}  // namespace crypto
}  // namespace tinywg
