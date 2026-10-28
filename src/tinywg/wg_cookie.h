// Cookie replies (whitepaper §5.4.7) for a responder that faces a flood of
// handshake initiations, e.g. on the Internet with a known public key. Each
// initiation costs Diffie-Hellman work; under load this guard answers with a
// cookie instead and handles only messages whose mac2 proves the sender owns
// its source address, at a limited rate per address.
//
//   tinywg::CookieGuard guard;
//   wg.setCookieGuard(&guard);
//
// Nothing of it is linked into a sketch that does not create one.
#pragma once

#include "wg_core.h"

namespace tinywg {

class CookieGuard : public HandshakeGuard {
public:
    // Under load once more than handshakesPerSecond handshake messages arrive
    // within a second. Under load, one source address gets one handled every
    // 1000 / perSourcePerSecond ms.
    explicit CookieGuard(uint8_t handshakesPerSecond = 4, uint8_t perSourcePerSecond = 2);

    Verdict check(const uint8_t *msg, size_t len, const Endpoint &from, uint32_t nowMs,
                  const uint8_t ourPublicKey[32], RandomFn random, uint8_t reply[64]) override;

private:
    static constexpr int kSources = 8;
    struct Source {
        uint8_t addr[4];
        bool used;
        uint32_t lastMs;
    };

    bool refreshSecret(uint32_t nowMs, RandomFn random);
    bool sourceAllowed(const Endpoint &from, uint32_t nowMs);

    uint8_t limit_;
    uint16_t sourceIntervalMs_;
    uint8_t count_ = 0;
    uint32_t windowMs_ = 0;
    bool hasSecret_ = false;
    uint32_t secretMs_ = 0;
    uint8_t secret_[32];
    Source sources_[kSources] = {};
};

}  // namespace tinywg
