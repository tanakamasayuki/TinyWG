// TinyWG as an lwIP network interface (wg0). Once begin() returns, anything
// built on lwIP (WiFiClient / NetworkClient, WiFiServer, an MQTT broker, ...)
// reaches the peers through the tunnel by their AllowedIPs addresses.
//
// Everything runs in lwIP's own context: the outer UDP socket's receive
// callback, the netif's output and a 100 ms lwIP timer. loop() has nothing to
// call. ESP32: the public methods take the lwIP core lock, so do not call them
// from inside an lwIP callback.
//
// setOuterUdp() moves the outer side to any Arduino UDP instead, for a network
// module with its own TCP/IP stack (W5500 with the Ethernet library, a modem's
// AT socket): wg0 stays in lwIP. The outer I/O then runs on its own (ESP32: a
// task, CH32V: lwIP's timer), or from update() in loop().
#pragma once

#include <Udp.h>

#include "TinyWGBase.h"

#if defined(ARDUINO_ARCH_CH32RV)
#include <Network.h>   // brings the Network library (and its lwIP) into the build
#endif

#if defined(ARDUINO_ARCH_RP2040) && defined(__FREERTOS)
// arduino-pico in FreeRTOS mode runs lwIP in a task of its own, which TinyWG
// does not support. TinyWGUdp, tinywg::Device and TinyWGTimeServer still work.
template <bool Supported = false>
struct TinyWGNotInFreeRtos {
    static_assert(Supported, "TinyWG (wg0 on lwIP) does not support arduino-pico's FreeRTOS mode: use TinyWGUdp");
};
using TinyWG = TinyWGNotInFreeRtos<>;
#elif __has_include(<lwip/netif.h>)
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
    // begin() and the peers from wg-quick style text ([Interface] PrivateKey,
    // Address, ListenPort, MTU; [Peer] PublicKey, PresharedKey, AllowedIPs,
    // Endpoint, PersistentKeepalive). IPv6 entries are skipped, other keys
    // ignored; an Endpoint must be an IPv4 address. false with *errorLine set to
    // the offending line (0: PrivateKey or Address missing, or begin() failed).
    // Not linked unless called.
    bool beginConfig(const char *text, int *errorLine = nullptr);

    // Sends and receives the outer datagrams through udp, opened on the listen
    // port by begin(). Call before begin(); nullptr goes back to lwIP's own UDP.
    // false while running.
    //
    // By default the outer I/O runs by itself, so the tunnel keeps going while
    // loop() blocks (a socket write waiting for ACKs, an MQTT connect): on ESP32
    // in a task of its own, on CH32V from lwIP's timer. The sketch must then not
    // use the module elsewhere at the same time. pollFromLoop = true leaves it
    // to update() in loop() instead, for a sketch that also uses the module
    // directly; arduino-pico always works this way. Then nothing in loop() may
    // block for long.
    bool setOuterUdp(UDP *udp, bool pollFromLoop = false);
    // With setOuterUdp(udp, true) or on arduino-pico: reads the datagrams that
    // arrived and sends the queued ones. Call from loop() often. Otherwise it
    // does nothing, so it is always safe to call.
    void update();

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
    void pollOuter();
    void receive(struct pbuf *p, const tinywg::Endpoint &from);
    bool outerInBackground() const;
    void startOuter();
    void stopOuter();
#if defined(ARDUINO_ARCH_ESP32)
    static void outerTask(void *arg);
#elif defined(ARDUINO_ARCH_CH32RV)
    static void outerTimer(void *arg);
#endif

    // Outer datagrams waiting for update() when the outer side is setOuterUdp():
    // the engine may send from lwIP's context, where the module must not be used.
    static constexpr int kOuterQueue = 8;
    struct Outgoing {
        struct pbuf *p;
        tinywg::Endpoint to;
    };

    struct netif netif_;
    struct udp_pcb *pcb_ = nullptr;
    bool up_ = false;
    UDP *outerUdp_ = nullptr;
    struct pbuf *rxPbuf_ = nullptr;   // the outer datagram being decrypted, while it can be handed on
    bool outerFromLoop_ = false;
#if defined(ARDUINO_ARCH_ESP32)
    void *outerTask_ = nullptr;           // TaskHandle_t
    volatile bool outerRun_ = false;
    volatile bool outerStopped_ = true;
#endif
    Outgoing outgoing_[kOuterQueue];
    int outgoingHead_ = 0;
    int outgoingCount_ = 0;
};

#endif
