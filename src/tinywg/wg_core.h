// The WireGuard protocol engine: handshake, sessions, timers, replay window
// and cryptokey routing. It depends on neither lwIP nor the Arduino API: it is
// fed outer UDP datagrams and inner IP packets, hands results to a Transport,
// and is driven by the caller's clock (milliseconds, wrap-safe).
//
// Not thread-safe: the caller serialises every call (SPEC.ja.md §3.1).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wg_config.h"
#include "wg_port.h"
#include "wg_state.h"

namespace tinywg {

constexpr size_t kKeyLen = 32;

// Where the engine's output goes.
class Transport {
public:
    virtual ~Transport() {}
    // Send one outer UDP datagram.
    virtual void sendOuter(const Endpoint &to, const uint8_t *data, size_t len) = 0;
    // An authenticated inner IPv4 packet from peer. The buffer is the one passed
    // to receiveOuter() and may be modified.
    virtual void deliverInner(int peer, uint8_t *packet, size_t len) = 0;
};

enum class SendResult : uint8_t {
    Sent,        // encrypted and handed to the transport
    Queued,      // held until the handshake completes (TINYWG_STAGED_PACKET)
    Dropped,     // no session yet and nowhere to hold it
    NoRoute,     // no peer's AllowedIPs covers the destination
    NoEndpoint,  // the peer has never been reached and has no configured endpoint
    Invalid,     // not an IPv4 packet, or larger than mtu()
};

struct PeerStatus {
    bool sessionUp;          // a session that can send right now
    bool handshakeInFlight;  // an initiation waiting for its response
    bool hasHandshake;       // a handshake has completed at least once
    uint32_t lastHandshakeMs;
    Endpoint endpoint;
    uint64_t rxBytes;
    uint64_t txBytes;
};

// Dropped-packet counters, device-wide.
struct Stats {
    uint32_t rxShort;          // too short or unknown message type
    uint32_t rxBadMac1;        // handshake with a wrong mac1
    uint32_t rxHandshakeFail;  // handshake that failed to decrypt / authenticate
    uint32_t rxUnknownPeer;    // initiation from a public key that is not configured
    uint32_t rxStaleTimestamp; // initiation replayed or not newer than the last
    uint32_t rxFlood;          // initiation faster than the per-peer limit
    uint32_t rxNoSession;      // data / response / cookie for an unknown index
    uint32_t rxDecryptFail;    // data that failed authentication
    uint32_t rxReplay;         // data counter outside the window or seen before
    uint32_t rxNotAllowed;     // inner source address not in the peer's AllowedIPs
    uint32_t rxNotIpv4;        // inner packet that is not a well-formed IPv4 packet
    uint32_t handshakesInitiated;
    uint32_t handshakesCompleted;
    uint32_t handshakesResponded;
};

class Device {
public:
    static constexpr uint16_t kMinMtu = 576;

    Device();
    ~Device() { end(); }

    // Starts the device with its X25519 private key. false when no random
    // source is available (see setRandom()).
    bool begin(const uint8_t privateKey[kKeyLen], Transport *transport);
    void end();

    void setRandom(RandomFn fn) { random_ = fn; }
    void setWallClock(WallClockFn fn) { wallClock_ = fn; }

    // Adds a peer and returns its index, or -1 when the table is full or the
    // key is already present. presharedKey may be null.
    int addPeer(const uint8_t publicKey[kKeyLen], const uint8_t *presharedKey = nullptr);
    bool removePeer(int peer);
    bool addAllowedIp(int peer, const uint8_t addr[4], uint8_t cidr);
    bool setEndpoint(int peer, const Endpoint &endpoint);
    // 0 turns persistent keepalive off.
    bool setPersistentKeepalive(int peer, uint16_t seconds);
    // Inner MTU, from kMinMtu up to TINYWG_MTU. Larger packets are not sent.
    bool setMtu(uint16_t mtu);
    uint16_t mtu() const { return mtu_; }

    int findPeer(const uint8_t publicKey[kKeyLen]) const;
    // The peer whose AllowedIPs best match addr (longest prefix), or -1.
    int routePeer(const uint8_t addr[4]) const;
    bool peerStatus(int peer, PeerStatus *out) const;
    const Stats &stats() const { return stats_; }
    const uint8_t *publicKey() const { return staticPublic_; }

    // One outer UDP datagram received on the listen port. The buffer is
    // decrypted in place.
    void receiveOuter(uint8_t *data, size_t len, const Endpoint &from, uint32_t nowMs);
    // One inner IPv4 packet to send; the peer is chosen from AllowedIPs.
    SendResult sendInner(const uint8_t *packet, size_t len, uint32_t nowMs);
    // Starts a handshake with peer now (normally the first packet does).
    bool connect(int peer, uint32_t nowMs);
    // Runs the timers. Call at least every 100 ms or so.
    void tick(uint32_t nowMs);

private:
    using Peer = detail::Peer;
    using Session = detail::Session;

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    Peer *peer(int index);
    const Peer *peer(int index) const;
    int indexOf(const Peer *p) const { return (int)(p - peers_); }

    bool randomBytes(uint8_t *out, size_t len);
    uint32_t newIndex();
    void tai64n(uint8_t out[12]);

    void handleInitiation(uint8_t *msg, size_t len, const Endpoint &from, uint32_t nowMs);
    void handleResponse(uint8_t *msg, size_t len, const Endpoint &from, uint32_t nowMs);
    void handleCookieReply(uint8_t *msg, size_t len, uint32_t nowMs);
    void handleData(uint8_t *msg, size_t len, const Endpoint &from, uint32_t nowMs);

    bool initiateHandshake(Peer *p, bool retry, uint32_t nowMs);
    SendResult sendData(Peer *p, const uint8_t *packet, size_t len, uint32_t nowMs);
    void sendStaged(Peer *p, bool keepaliveIfEmpty, uint32_t nowMs);
    void addMacs(Peer *p, uint8_t *msg, size_t len, uint32_t nowMs);
    void sessionDerived(Peer *p, uint32_t nowMs);
    void handshakeComplete(Peer *p, uint32_t nowMs);
    void anySent(Peer *p, uint32_t nowMs);
    void anyReceived(Peer *p, uint32_t nowMs);
    void wipePeerKeys(Peer *p);

    Transport *transport_;
    RandomFn random_;
    WallClockFn wallClock_;
    bool started_;
    uint16_t mtu_;

    uint8_t staticPrivate_[kKeyLen];
    uint8_t staticPublic_[kKeyLen];
    uint8_t mac1Key_[kKeyLen];      // HASH(LABEL_MAC1 || our public key), checks incoming mac1
    uint8_t initialHash_[kKeyLen];  // HASH(HASH(CONSTRUCTION) || IDENTIFIER)
    uint64_t lastTaiSeconds_;
    uint32_t lastTaiNanos_;

    Peer peers_[TINYWG_MAX_PEERS];
    Stats stats_;

    // Outgoing message buffer: data header + padded payload + tag.
    uint8_t tx_[16 + TINYWG_MTU + 16 + 16];
#if TINYWG_STAGED_PACKET
    int stagedPeer_;
    size_t stagedLen_;
    uint8_t staged_[TINYWG_MTU];
#endif
};

// Key helpers for sketches.
bool generatePrivateKey(uint8_t privateKey[kKeyLen], RandomFn random = nullptr);
void derivePublicKey(uint8_t publicKey[kKeyLen], const uint8_t privateKey[kKeyLen]);
// Standard Base64 of 32 bytes (44 characters + NUL). false on malformed input.
bool keyFromBase64(uint8_t key[kKeyLen], const char *base64);
void keyToBase64(char out[45], const uint8_t key[kKeyLen]);

}  // namespace tinywg
