#include "wg_cookie.h"

#include <string.h>

#include "wg_crypto.h"

namespace tinywg {

namespace {

constexpr uint32_t kSecretLifetimeMs = 120000;   // COOKIE_SECRET_MAX_AGE
const char kLabelCookie[] = "cookie--";

}  // namespace

CookieGuard::CookieGuard(uint8_t handshakesPerSecond, uint8_t perSourcePerSecond)
    : limit_(handshakesPerSecond),
      sourceIntervalMs_(perSourcePerSecond ? 1000 / perSourcePerSecond : 1000)
{
}

bool CookieGuard::refreshSecret(uint32_t now, RandomFn random)
{
    if (hasSecret_ && (now - secretMs_) < kSecretLifetimeMs) return true;
    if (!random || !random(secret_, sizeof(secret_))) return false;
    hasSecret_ = true;
    secretMs_ = now;
    return true;
}

bool CookieGuard::sourceAllowed(const Endpoint &from, uint32_t now)
{
    Source *slot = nullptr;
    for (Source &s : sources_) {
        if (s.used && memcmp(s.addr, from.addr, 4) == 0) {
            slot = &s;
            break;
        }
    }
    if (slot) {
        if ((now - slot->lastMs) < sourceIntervalMs_) return false;
    } else {
        // A free entry, or else the one heard from longest ago.
        slot = &sources_[0];
        for (Source &s : sources_) {
            if (!s.used) {
                slot = &s;
                break;
            }
            if ((now - s.lastMs) > (now - slot->lastMs)) slot = &s;
        }
        memcpy(slot->addr, from.addr, 4);
        slot->used = true;
    }
    slot->lastMs = now;
    return true;
}

HandshakeGuard::Verdict CookieGuard::check(const uint8_t *msg, size_t len, const Endpoint &from, uint32_t now,
                                           const uint8_t ourPublicKey[32], RandomFn random, uint8_t reply[64])
{
    if ((now - windowMs_) >= 1000) {
        windowMs_ = now;
        count_ = 0;
    }
    if (count_ < 255) count_++;
    if (count_ <= limit_) return Accept;

    // Under load. The cookie for this source: MAC(secret, address || port).
    if (!refreshSecret(now, random)) return Drop;
    uint8_t source[6], cookie[16], expected[16];
    memcpy(source, from.addr, 4);
    source[4] = (uint8_t)(from.port >> 8);
    source[5] = (uint8_t)from.port;
    crypto::mac(cookie, secret_, sizeof(secret_), source, sizeof(source));
    crypto::mac(expected, cookie, sizeof(cookie), msg, len - 16);
    if (crypto::equal16(expected, msg + len - 16)) return sourceAllowed(from, now) ? Accept : Drop;

    // No valid mac2: send the cookie, sealed to the sender's mac1.
    uint8_t key[32];
    crypto::hash(key, (const uint8_t *)kLabelCookie, 8, ourPublicKey, 32);
    memset(reply, 0, 64);
    reply[0] = 3;
    memcpy(reply + 4, msg + 4, 4);   // receiver: the sender's index
    if (!random(reply + 8, 24)) return Drop;
    crypto::xaeadEncrypt(reply + 32, key, reply + 8, cookie, sizeof(cookie), msg + len - 32, 16);
    crypto::wipe(key, sizeof(key));
    return SendCookie;
}

}  // namespace tinywg
