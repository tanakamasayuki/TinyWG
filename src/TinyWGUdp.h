// TinyWG over any Arduino UDP (WiFiUDP, EthernetUDP, NetworkUDP, the host
// core's socket UDP). The tunnel carries raw IPv4 packets that the sketch
// builds and parses itself: there is no IP stack behind it. For a tunnel the
// rest of the firmware can use through lwIP, use TinyWG (TinyWG.h) instead.
#pragma once

#include <Udp.h>

#include "TinyWGBase.h"

class TinyWGUdp : public TinyWGBase, private tinywg::Transport {
public:
    // An inner IPv4 packet from peer. packet may be modified (e.g. turned into a reply).
    typedef void (*PacketHandler)(int peer, uint8_t *packet, size_t len, void *context);

    // udp is opened on listenPort here. privateKeyBase64 as printed by `wg genkey`.
    bool begin(UDP &udp, uint16_t listenPort, const char *privateKeyBase64);
    bool begin(UDP &udp, uint16_t listenPort, const uint8_t privateKey[32]);
    void end();

    void onPacket(PacketHandler handler, void *context = nullptr);
    // Sends one inner IPv4 packet; the peer is chosen from AllowedIPs.
    tinywg::SendResult send(const uint8_t *packet, size_t len);
    // Reads the pending datagrams and runs the timers. Call from loop().
    void update();

private:
    void sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len) override;
    void deliverInner(int peer, uint8_t *packet, size_t len) override;

    UDP *udp_ = nullptr;
    PacketHandler handler_ = nullptr;
    void *context_ = nullptr;
    uint8_t rx_[16 + TINYWG_MTU + 16 + 16];
};
