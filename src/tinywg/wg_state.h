// Per-peer protocol state of the WG core. Internal: sketches use the Device API.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wg_config.h"

namespace tinywg {

// An outer UDP endpoint (IPv4). port 0 means "unknown".
struct Endpoint {
    uint8_t addr[4];
    uint16_t port;
};

namespace detail {

// A timer that is either off or due at a millisecond tick (wrap-safe).
struct Timer {
    bool armed;
    uint32_t due;

    void arm(uint32_t at) { armed = true; due = at; }
    void disarm() { armed = false; }
    bool expired(uint32_t now) const { return armed && (int32_t)(now - due) >= 0; }
};

// Sliding window over received data counters (RFC 6479 style ring of words).
// Counters up to (TINYWG_REPLAY_WINDOW - 32) behind the greatest are accepted
// once each.
struct ReplayWindow {
    static constexpr uint32_t kWords = TINYWG_REPLAY_WINDOW / 32;
    static constexpr uint64_t kSpan = TINYWG_REPLAY_WINDOW - 32;

    bool any;
    uint64_t greatest;
    uint32_t words[kWords];

    void reset();
    bool acceptable(uint64_t counter) const;
    void mark(uint64_t counter);
};

struct Session {
    bool valid;
    bool initiator;   // we sent the initiation that created it
    uint32_t localIndex;
    uint32_t remoteIndex;
    uint32_t birthMs;
    uint64_t sendCounter;
    uint8_t sendKey[32];
    uint8_t recvKey[32];
    ReplayWindow replay;
};

// The initiator's half-finished handshake.
struct Handshake {
    bool initiationSent;
    uint32_t localIndex;
    uint8_t ephemeralPrivate[32];
    uint8_t hash[32];
    uint8_t chainKey[32];
};

struct AllowedIp {
    uint8_t addr[4];
    uint8_t cidr;
};

struct Peer {
    bool used;
    uint8_t publicKey[32];
    uint8_t presharedKey[32];   // zero when none
    uint8_t staticShared[32];   // DH(our static, peer static), computed once in addPeer()
    uint8_t mac1Key[32];        // HASH(LABEL_MAC1 || peer public key), for mac1 we send
    uint8_t cookieKey[32];      // HASH(LABEL_COOKIE || peer public key), opens its cookie replies
    AllowedIp allowed[TINYWG_MAX_ALLOWED_IPS];
    uint8_t allowedCount;
    Endpoint endpoint;
    uint16_t keepaliveSeconds;  // persistent keepalive, 0 = off

    uint8_t lastTimestamp[12];  // greatest TAI64N accepted from this peer
    bool initiationReceived;
    uint32_t lastInitiationRxMs;

    Handshake handshake;
    Session current;
    Session previous;
    Session next;               // responder side: derived but not yet confirmed by the initiator

    uint8_t cookie[16];
    bool hasCookie;
    uint32_t cookieMs;
    uint8_t lastMac1[16];       // mac1 of the last handshake message we sent
    bool hasLastMac1;

    bool attempting;            // initiations being sent until a session comes up
    uint32_t attemptStartMs;
    bool initiationSentOnce;
    uint32_t lastInitiationTxMs;
    Timer retransmit;
    Timer passiveKeepalive;     // we received data and have not sent anything since
    Timer newHandshake;         // we sent data and have not received anything since
    Timer persistentKeepalive;
    Timer zeroKeys;
    bool lastMinuteHandshake;   // already rekeyed because the session is about to be rejected

    bool hasHandshake;
    uint32_t lastHandshakeMs;
    uint64_t rxBytes;
    uint64_t txBytes;
};

}  // namespace detail
}  // namespace tinywg
