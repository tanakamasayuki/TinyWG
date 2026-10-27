#include "TinyWGUdp.h"

#include <string.h>

bool TinyWGUdp::begin(UDP &udp, uint16_t listenPort, const char *privateKeyBase64)
{
    uint8_t key[32];
    if (!tinywg::keyFromBase64(key, privateKeyBase64)) return false;
    bool ok = begin(udp, listenPort, key);
    memset(key, 0, sizeof(key));
    return ok;
}

bool TinyWGUdp::begin(UDP &udp, uint16_t listenPort, const uint8_t privateKey[32])
{
    end();
    if (!device_.begin(privateKey, this)) return false;
    if (!udp.begin(listenPort)) {
        device_.end();
        return false;
    }
    udp_ = &udp;
    return true;
}

void TinyWGUdp::end()
{
    if (udp_) udp_->stop();
    udp_ = nullptr;
    device_.end();
}

void TinyWGUdp::onPacket(PacketHandler handler, void *context)
{
    handler_ = handler;
    context_ = context;
}

tinywg::SendResult TinyWGUdp::send(const uint8_t *packet, size_t len)
{
    return device_.sendInner(packet, len, millis());
}

void TinyWGUdp::update()
{
    if (!udp_) return;
    for (int n; (n = udp_->parsePacket()) > 0;) {
        if ((size_t)n > sizeof(rx_)) {
            udp_->flush();   // larger than any message we accept
            continue;
        }
        IPAddress ip = udp_->remoteIP();
        tinywg::Endpoint from = {{ip[0], ip[1], ip[2], ip[3]}, udp_->remotePort()};
        int len = udp_->read(rx_, sizeof(rx_));
        if (len > 0) device_.receiveOuter(rx_, (size_t)len, from, millis());
    }
    device_.tick(millis());
}

void TinyWGUdp::sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len)
{
    udp_->beginPacket(IPAddress(to.addr[0], to.addr[1], to.addr[2], to.addr[3]), to.port);
    udp_->write(data, len);
    udp_->endPacket();
}

void TinyWGUdp::deliverInner(int peer, uint8_t *packet, size_t len)
{
    if (handler_) handler_(peer, packet, len, context_);
}
