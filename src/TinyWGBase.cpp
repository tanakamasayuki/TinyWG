#include "TinyWGBase.h"

#include <string.h>

int TinyWGBase::addPeer(const char *publicKeyBase64, const char *presharedKeyBase64)
{
    uint8_t pub[32], psk[32];
    if (!tinywg::keyFromBase64(pub, publicKeyBase64)) return -1;
    if (presharedKeyBase64 && !tinywg::keyFromBase64(psk, presharedKeyBase64)) return -1;
    int index = addPeer(pub, presharedKeyBase64 ? psk : nullptr);
    memset(psk, 0, sizeof(psk));
    return index;
}

int TinyWGBase::addPeer(const uint8_t publicKey[32], const uint8_t *presharedKey)
{
    lock();
    int index = device_.addPeer(publicKey, presharedKey);
    unlock();
    return index;
}

bool TinyWGBase::removePeer(int peer)
{
    lock();
    bool ok = device_.removePeer(peer);
    unlock();
    return ok;
}

bool TinyWGBase::addAllowedIp(int peer, const IPAddress &address, uint8_t cidr)
{
    uint8_t addr[4] = {address[0], address[1], address[2], address[3]};
    lock();
    bool ok = device_.addAllowedIp(peer, addr, cidr);
    unlock();
    return ok;
}

bool TinyWGBase::setEndpoint(int peer, const IPAddress &address, uint16_t port)
{
    tinywg::Endpoint e = {{address[0], address[1], address[2], address[3]}, port};
    lock();
    bool ok = device_.setEndpoint(peer, e);
    unlock();
    return ok;
}

bool TinyWGBase::setPersistentKeepalive(int peer, uint16_t seconds)
{
    lock();
    bool ok = device_.setPersistentKeepalive(peer, seconds);
    unlock();
    return ok;
}

bool TinyWGBase::setMtu(uint16_t mtu)
{
    lock();
    bool ok = device_.setMtu(mtu);
    unlock();
    return ok;
}

void TinyWGBase::setTimestampReset(uint16_t seconds)
{
    lock();
    device_.setTimestampReset(seconds);
    unlock();
}

bool TinyWGBase::connect(int peer)
{
    lock();
    bool ok = device_.connect(peer, millis());
    unlock();
    return ok;
}

bool TinyWGBase::peerUp(int peer)
{
    tinywg::PeerStatus s;
    return peerStatus(peer, &s) && s.sessionUp;
}

bool TinyWGBase::peerStatus(int peer, tinywg::PeerStatus *out)
{
    lock();
    bool ok = device_.peerStatus(peer, out);
    unlock();
    return ok;
}

tinywg::Stats TinyWGBase::stats()
{
    lock();
    tinywg::Stats s = device_.stats();
    unlock();
    return s;
}

void TinyWGBase::publicKey(char out[45]) const { tinywg::keyToBase64(out, device_.publicKey()); }
