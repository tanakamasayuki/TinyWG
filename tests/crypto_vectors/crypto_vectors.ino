// Primitive outputs for fixed inputs, checked by the Python side against
// hashlib / cryptography (independent implementations), plus the checks that
// need no reference: AEAD round trip and tamper detection, low-order points,
// Base64 round trip.
//
// Inputs are pattern(n, seed): byte i = (i * 7 + seed) & 0xFF.
// Output: "VEC <name> <hex>" lines, PASS/FAIL lines, then "TEST done N/M".

#include <TinyWG.h>
#include <tinywg/blake2s.h>
#include <tinywg/wg_crypto.h>
#include <string.h>

using namespace tinywg;

static int g_pass = 0;
static int g_total = 0;

#define EXPECT_TRUE(name, cond)      \
    do                               \
    {                                \
        ++g_total;                   \
        if (cond)                    \
        {                            \
            ++g_pass;                \
            Serial.print("PASS ");   \
            Serial.println(name);    \
        }                            \
        else                         \
        {                            \
            Serial.print("FAIL ");   \
            Serial.print(name);      \
            Serial.println(" cond"); \
        }                            \
    } while (0)

static void pattern(uint8_t *out, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(i * 7 + seed);
}

static void printVec(const char *name, const uint8_t *data, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    Serial.print("VEC ");
    Serial.print(name);
    Serial.print(' ');
    for (size_t i = 0; i < len; i++) {
        Serial.print(hex[data[i] >> 4]);
        Serial.print(hex[data[i] & 15]);
    }
    if (len == 0) Serial.print('-');
    Serial.println();
}

static uint8_t in[300];
static uint8_t buf[300 + 16];

void setup()
{
    Serial.begin(115200);
    delay(5000);
    char name[48];

    // BLAKE2s: unkeyed 32-byte digests, keyed 16-byte MACs, across block boundaries.
    static const size_t kLens[] = {0, 3, 63, 64, 65, 128, 129, 300};
    uint8_t key[32], out[32];
    pattern(key, 32, 1);
    for (size_t n : kLens) {
        pattern(in, n, 2);
        crypto::hash(out, in, n);
        snprintf(name, sizeof(name), "blake2s/%u", (unsigned)n);
        printVec(name, out, 32);
        crypto::mac(out, key, 32, in, n);
        snprintf(name, sizeof(name), "mac32/%u", (unsigned)n);
        printVec(name, out, 16);
        crypto::mac(out, key, 16, in, n);
        snprintf(name, sizeof(name), "mac16/%u", (unsigned)n);
        printVec(name, out, 16);
    }
    // Streaming in 7-byte pieces equals one call.
    {
        tinywg_blake2s_ctx ctx;
        uint8_t a[32], b[32];
        pattern(in, 300, 2);
        tinywg_blake2s_init(&ctx, 32, nullptr, 0);
        for (size_t i = 0; i < 300; i += 7) tinywg_blake2s_update(&ctx, in + i, 300 - i < 7 ? 300 - i : 7);
        tinywg_blake2s_final(&ctx, a);
        tinywg_blake2s(b, 32, nullptr, 0, in, 300);
        EXPECT_TRUE("blake2s streaming", memcmp(a, b, 32) == 0);
    }

    // KDF1..3 with an empty input and a 32-byte input.
    static const size_t kKdfLens[] = {0, 32};
    for (size_t n : kKdfLens) {
        uint8_t t1[32], t2[32], t3[32];
        pattern(in, n, 3);
        crypto::kdf(t1, t2, t3, key, in, n);
        snprintf(name, sizeof(name), "kdf/%u/1", (unsigned)n);
        printVec(name, t1, 32);
        snprintf(name, sizeof(name), "kdf/%u/2", (unsigned)n);
        printVec(name, t2, 32);
        snprintf(name, sizeof(name), "kdf/%u/3", (unsigned)n);
        printVec(name, t3, 32);
    }

    // X25519: public keys of two clamped private keys and their shared secret.
    {
        uint8_t a[32], b[32], pa[32], pb[32], s1[32], s2[32];
        pattern(a, 32, 4);
        pattern(b, 32, 5);
        crypto::clampPrivateKey(a);
        crypto::clampPrivateKey(b);
        crypto::publicKey(pa, a);
        crypto::publicKey(pb, b);
        printVec("x25519/pub_a", pa, 32);
        printVec("x25519/pub_b", pb, 32);
        EXPECT_TRUE("x25519 dh a", crypto::dh(s1, a, pb));
        EXPECT_TRUE("x25519 dh b", crypto::dh(s2, b, pa));
        printVec("x25519/shared", s1, 32);
        EXPECT_TRUE("x25519 agree", memcmp(s1, s2, 32) == 0);
        uint8_t lowOrder[32] = {0};   // the point 0 has order 1
        EXPECT_TRUE("x25519 rejects low order", !crypto::dh(s1, a, lowOrder));
    }

    // ChaCha20-Poly1305 with the WireGuard nonce layout.
    {
        uint8_t k[32], ad[32];
        pattern(k, 32, 6);
        pattern(ad, 32, 7);
        static const size_t kAeadLens[] = {0, 1, 16, 100, 300};
        static const uint64_t kCounters[] = {0, 0x0102030405060708ull};
        for (uint64_t c : kCounters) {
            for (size_t n : kAeadLens) {
                pattern(in, n, 8);
                crypto::aeadEncrypt(buf, k, c, in, n, ad, 32);
                snprintf(name, sizeof(name), "aead/%u/%u", (unsigned)(c & 0xFF), (unsigned)n);
                printVec(name, buf, n + 16);
                uint8_t plain[300];
                EXPECT_TRUE("aead open", crypto::aeadDecrypt(plain, k, c, buf, n, ad, 32) && memcmp(plain, in, n) == 0);
                buf[n / 2] ^= 0x01;
                EXPECT_TRUE("aead tamper", !crypto::aeadDecrypt(plain, k, c, buf, n, ad, 32));
            }
        }
        // In place, as the data path uses it.
        pattern(buf, 100, 8);
        crypto::aeadEncrypt(buf, k, 5, buf, 100, nullptr, 0);
        EXPECT_TRUE("aead in place", crypto::aeadDecrypt(buf, k, 5, buf, 100, nullptr, 0));
        pattern(in, 100, 8);
        EXPECT_TRUE("aead in place content", memcmp(buf, in, 100) == 0);
    }

    // XChaCha20-Poly1305 round trip (cookie replies).
    {
        uint8_t k[32], nonce[24], cookie[16], sealed[32], opened[16], ad[16];
        pattern(k, 32, 9);
        pattern(nonce, 24, 10);
        pattern(cookie, 16, 11);
        pattern(ad, 16, 12);
        crypto::xaeadEncrypt(sealed, k, nonce, cookie, 16, ad, 16);
        printVec("xaead", sealed, 32);
        EXPECT_TRUE("xaead open", crypto::xaeadDecrypt(opened, k, nonce, sealed, 16, ad, 16) &&
                                      memcmp(opened, cookie, 16) == 0);
        ad[0] ^= 1;
        EXPECT_TRUE("xaead wrong ad", !crypto::xaeadDecrypt(opened, k, nonce, sealed, 16, ad, 16));
    }

    // Base64 keys.
    {
        uint8_t k[32], back[32];
        char text[45];
        pattern(k, 32, 13);
        keyToBase64(text, k);
        Serial.print("B64 ");
        Serial.println(text);
        EXPECT_TRUE("base64 round trip", keyFromBase64(back, text) && memcmp(back, k, 32) == 0);
        EXPECT_TRUE("base64 rejects short", !keyFromBase64(back, "AAAA"));
        text[10] = '*';
        EXPECT_TRUE("base64 rejects bad char", !keyFromBase64(back, text));
    }

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print('/');
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
