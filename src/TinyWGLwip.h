// TinyWG as an lwIP network interface (wg0). Once begin() returns, anything
// built on lwIP (WiFiClient / NetworkClient, WiFiServer, an MQTT broker, ...)
// reaches the peers through the tunnel by their AllowedIPs addresses.
//
// Everything runs in lwIP's own context: the outer UDP socket's receive
// callback, the netif's output and a 100 ms lwIP timer. loop() has nothing to
// call. ESP32: the public methods take the lwIP core lock, so do not call them
// from inside an lwIP callback.
#pragma once

#include "TinyWGBase.h"

#if defined(ARDUINO_ARCH_CH32RV)
#include <Network.h>   // brings the Network library (and its lwIP) into the build
#endif

#if __has_include(<lwip/netif.h>)
#define TINYWG_HAS_LWIP 1

#include <lwip/netif.h>
#include <lwip/pbuf.h>
#include <lwip/udp.h>

class TinyWG : public TinyWGBase, private tinywg::Transport {
public:
    // Brings up wg0 with our tunnel address and subnet, and opens the outer UDP
    // port. privateKeyBase64 as printed by `wg genkey`.
    bool begin(const IPAddress &localIp, const IPAddress &subnet, const char *privateKeyBase64,
               uint16_t listenPort = 51820);
    bool begin(const IPAddress &localIp, const IPAddress &subnet, const uint8_t privateKey[32],
               uint16_t listenPort = 51820);
    void end();

    // Also sets wg0's MTU, so lwIP sizes TCP segments to fit. Any time, before begin() too.
    bool setMtu(uint16_t mtu) override;

    // wg0, for routing or NAPT set-up done by the sketch.
    struct netif *netif() { return &netif_; }

#if defined(IP_NAPT) && IP_NAPT
    // ESP32: translate what peers send beyond wg0 (to the LAN or the Internet)
    // to the address of the interface it leaves by.
    bool enableNapt(bool enable = true);
#endif

private:
    void lock() override;
    void unlock() override;

    void sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len) override;
    void deliverInner(int peer, uint8_t *packet, size_t len) override;

    static err_t netifInit(struct netif *n);
    static err_t netifOutput(struct netif *n, struct pbuf *p, const ip4_addr_t *dest);
    static void udpReceive(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
    static void timer(void *arg);

    struct netif netif_;
    struct udp_pcb *pcb_ = nullptr;
    bool up_ = false;
    // For pbuf chains that are not contiguous. Outer receive and inner send never
    // overlap in it: an inner packet is copied out before lwIP sees it.
    uint8_t scratch_[16 + TINYWG_MTU + 16 + 16];
};

#endif
