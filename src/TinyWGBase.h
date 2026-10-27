// Peer configuration shared by the Arduino front ends (TinyWG, TinyWGUdp).
#pragma once

#include <Arduino.h>
#include <IPAddress.h>

#include "tinywg/wg_core.h"

class TinyWGBase {
public:
    virtual ~TinyWGBase() {}

    // Adds a peer by its Base64 public key (as printed by `wg pubkey`), with an
    // optional Base64 preshared key. Returns the peer index, or -1.
    int addPeer(const char *publicKeyBase64, const char *presharedKeyBase64 = nullptr);
    int addPeer(const uint8_t publicKey[32], const uint8_t *presharedKey = nullptr);
    bool removePeer(int peer);
    // One AllowedIPs entry, e.g. addAllowedIp(peer, IPAddress(10, 0, 0, 2), 32).
    bool addAllowedIp(int peer, const IPAddress &address, uint8_t cidr);
    // Where to send the handshake. Optional for a peer that always connects to us.
    bool setEndpoint(int peer, const IPAddress &address, uint16_t port);
    // Keepalive interval in seconds for a peer behind NAT; 0 = off.
    bool setPersistentKeepalive(int peer, uint16_t seconds);
    // Inner MTU, 576 up to TINYWG_MTU (1420 by default). Lower it when the outer
    // path is smaller than 1500 (PPPoE, LTE, another tunnel): MTU + 60 must fit.
    virtual bool setMtu(uint16_t mtu);
    uint16_t mtu() const { return device_.mtu(); }
    // Starts a handshake now instead of on the first packet.
    bool connect(int peer);

    bool peerUp(int peer);
    bool peerStatus(int peer, tinywg::PeerStatus *out);
    tinywg::Stats stats();
    // Our public key in Base64 (44 characters + NUL), for the remote [Peer] section.
    void publicKey(char out[45]) const;

    // Replaces the random source or the TAI64N wall clock (wg_port.h).
    void setRandom(tinywg::RandomFn fn) { device_.setRandom(fn); }
    void setWallClock(tinywg::WallClockFn fn) { device_.setWallClock(fn); }

    // The protocol engine, for what the wrapper does not expose. Calls into it
    // must hold lock() on cores where the network stack runs in another task.
    tinywg::Device &device() { return device_; }

protected:
    // Serialises against the network stack (TinyWG on ESP32: the lwIP core lock).
    virtual void lock() {}
    virtual void unlock() {}

    tinywg::Device device_;
};
