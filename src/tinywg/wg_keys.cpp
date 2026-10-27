#include <string.h>

#include "wg_core.h"
#include "wg_crypto.h"

namespace tinywg {

bool generatePrivateKey(uint8_t privateKey[kKeyLen], RandomFn random)
{
    if (!random) random = port::defaultRandom();
    if (!random || !random(privateKey, kKeyLen)) return false;
    crypto::clampPrivateKey(privateKey);
    return true;
}

void derivePublicKey(uint8_t publicKey[kKeyLen], const uint8_t privateKey[kKeyLen])
{
    uint8_t clamped[kKeyLen];
    memcpy(clamped, privateKey, kKeyLen);
    crypto::clampPrivateKey(clamped);
    crypto::publicKey(publicKey, clamped);
    crypto::wipe(clamped, sizeof(clamped));
}

static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int decodeChar(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// 32 bytes are 43 Base64 characters and one '=' of padding.
bool keyFromBase64(uint8_t key[kKeyLen], const char *base64)
{
    if (!base64 || strlen(base64) != 44 || base64[43] != '=') return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t out = 0;
    for (int i = 0; i < 43; i++) {
        int v = decodeChar(base64[i]);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            key[out++] = (uint8_t)(acc >> bits);
        }
    }
    // The 2 bits left over must be zero for a canonical encoding.
    return out == kKeyLen && (acc & ((1u << bits) - 1)) == 0;
}

void keyToBase64(char out[45], const uint8_t key[kKeyLen])
{
    size_t o = 0;
    for (size_t i = 0; i < 30; i += 3) {
        uint32_t v = ((uint32_t)key[i] << 16) | ((uint32_t)key[i + 1] << 8) | key[i + 2];
        out[o++] = kAlphabet[(v >> 18) & 63];
        out[o++] = kAlphabet[(v >> 12) & 63];
        out[o++] = kAlphabet[(v >> 6) & 63];
        out[o++] = kAlphabet[v & 63];
    }
    uint32_t v = ((uint32_t)key[30] << 16) | ((uint32_t)key[31] << 8);
    out[o++] = kAlphabet[(v >> 18) & 63];
    out[o++] = kAlphabet[(v >> 12) & 63];
    out[o++] = kAlphabet[(v >> 6) & 63];
    out[o++] = '=';
    out[o] = '\0';
}

}  // namespace tinywg
