#include "TinyWGLwip.h"

#ifdef TINYWG_HAS_LWIP

#include <lwip/ip4.h>
#include <lwip/timeouts.h>
#include <string.h>

#if defined(ARDUINO_ARCH_RP2040)
#include <lwip_wrap.h>
#include <new>
#elif !NO_SYS && LWIP_TCPIP_CORE_LOCKING
#include <lwip/tcpip.h>
#endif
#if defined(IP_NAPT) && IP_NAPT
#include <lwip/lwip_napt.h>
#endif

static constexpr uint32_t kTimerMs = 100;
#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
static constexpr uint32_t kOuterTaskStack = 4096;
static constexpr UBaseType_t kOuterTaskPriority = 2;   // above loop() (1), below lwIP
#elif defined(ARDUINO_ARCH_CH32RV)
static constexpr uint32_t kOuterPollMs = 1;
#endif

// ESP32: the lwIP core lock. arduino-pico: the same recursive lock its lwIP
// wrappers take (LWIPMutex has no state, so constructing and destroying one
// is the lock and the unlock). CH32V: lwIP runs only from yield(), no lock.
#if defined(ARDUINO_ARCH_RP2040)
static uint8_t lwipMutexStorage[sizeof(LWIPMutex)];
#endif

void TinyWG::lock()
{
#if defined(ARDUINO_ARCH_RP2040)
    new (lwipMutexStorage) LWIPMutex();
#elif !NO_SYS && LWIP_TCPIP_CORE_LOCKING
    LOCK_TCPIP_CORE();
#endif
}

void TinyWG::unlock()
{
#if defined(ARDUINO_ARCH_RP2040)
    reinterpret_cast<LWIPMutex *>(lwipMutexStorage)->~LWIPMutex();
#elif !NO_SYS && LWIP_TCPIP_CORE_LOCKING
    UNLOCK_TCPIP_CORE();
#endif
}

bool TinyWG::begin(const IPAddress &localIp, const IPAddress &subnet, const char *privateKeyBase64,
                   uint16_t listenPort)
{
    uint8_t key[32];
    if (!tinywg::keyFromBase64(key, privateKeyBase64)) return false;
    bool ok = begin(localIp, subnet, key, listenPort);
    memset(key, 0, sizeof(key));
    return ok;
}

bool TinyWG::setOuterUdp(UDP *udp, bool pollFromLoop)
{
    if (up_) return false;
    outerUdp_ = udp;
    outerFromLoop_ = pollFromLoop;
    return true;
}

bool TinyWG::outerInBackground() const
{
#if defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_CH32RV)
    return outerUdp_ && !outerFromLoop_;
#else
    return false;
#endif
}

// Called once wg0 is up, without the lock.
void TinyWG::startOuter()
{
    if (!outerInBackground()) return;
#if defined(ARDUINO_ARCH_ESP32)
    outerRun_ = true;
    outerStopped_ = false;
    TaskHandle_t t = nullptr;
    if (xTaskCreate(outerTask, "tinywg", kOuterTaskStack, this, kOuterTaskPriority, &t) != pdPASS) {
        outerRun_ = false;
        outerStopped_ = true;
    }
    outerTask_ = t;
#elif defined(ARDUINO_ARCH_CH32RV)
    sys_timeout(kOuterPollMs, outerTimer, this);
#endif
}

// Waits until the background I/O no longer touches the module. Without the lock.
void TinyWG::stopOuter()
{
    if (!outerInBackground()) return;
#if defined(ARDUINO_ARCH_ESP32)
    outerRun_ = false;
    while (!outerStopped_) vTaskDelay(1);
    outerTask_ = nullptr;
#elif defined(ARDUINO_ARCH_CH32RV)
    sys_untimeout(outerTimer, this);
#endif
}

#if defined(ARDUINO_ARCH_ESP32)
void TinyWG::outerTask(void *arg)
{
    TinyWG *self = static_cast<TinyWG *>(arg);
    while (self->outerRun_) {
        self->pollOuter();
        vTaskDelay(1);
    }
    self->outerStopped_ = true;
    vTaskDelete(nullptr);
}
#elif defined(ARDUINO_ARCH_CH32RV)
// lwIP's timers run in loop()'s context on CH32V, also inside a blocking socket call.
void TinyWG::outerTimer(void *arg)
{
    TinyWG *self = static_cast<TinyWG *>(arg);
    self->pollOuter();
    sys_timeout(kOuterPollMs, outerTimer, self);
}
#endif

bool TinyWG::begin(const IPAddress &localIp, const IPAddress &subnet, const uint8_t privateKey[32],
                   uint16_t listenPort)
{
    end();
    // Outside the lock: the UDP may be one on lwIP's sockets (ESP32), which take it themselves.
    if (outerUdp_ && !outerUdp_->begin(listenPort)) return false;
    lock();
    bool ok = device_.begin(privateKey, this);
    if (ok) {
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip, localIp[0], localIp[1], localIp[2], localIp[3]);
        IP4_ADDR(&mask, subnet[0], subnet[1], subnet[2], subnet[3]);
        ip4_addr_set_zero(&gw);
        memset(&netif_, 0, sizeof(netif_));
        ok = netif_add(&netif_, &ip, &mask, &gw, this, netifInit, ip_input) != nullptr;
    }
    if (ok && !outerUdp_) {
        netif_set_up(&netif_);
        netif_set_link_up(&netif_);
        pcb_ = udp_new();
        ok = pcb_ && udp_bind(pcb_, IP4_ADDR_ANY, listenPort) == ERR_OK;
        if (!ok) {
            if (pcb_) udp_remove(pcb_);
            pcb_ = nullptr;
            netif_remove(&netif_);
        }
    }
    if (ok && outerUdp_) {
        netif_set_up(&netif_);
        netif_set_link_up(&netif_);
    } else if (ok) {
        udp_recv(pcb_, udpReceive, this);
    }
    if (ok) {
        sys_timeout(kTimerMs, timer, this);
        up_ = true;
    } else {
        device_.end();
    }
    unlock();
    if (!ok && outerUdp_) outerUdp_->stop();
    if (ok) startOuter();
    return ok;
}

void TinyWG::end()
{
    if (!up_) {
        // Nothing of lwIP's to undo, and its lock may not exist yet (ESP32 before any network start).
        device_.end();
        return;
    }
    stopOuter();
    lock();
    bool wasUp = up_;
    if (up_) {
        sys_untimeout(timer, this);
        if (pcb_) udp_remove(pcb_);
        pcb_ = nullptr;
        netif_remove(&netif_);
        up_ = false;
    }
    for (; outgoingCount_; outgoingCount_--, outgoingHead_ = (outgoingHead_ + 1) % kOuterQueue)
        pbuf_free(outgoing_[outgoingHead_].p);
    device_.end();
    unlock();
    if (wasUp && outerUdp_) outerUdp_->stop();
}

void TinyWG::update()
{
    if (up_ && outerUdp_ && !outerInBackground()) pollOuter();
}

void TinyWG::pollOuter()
{
    // The module is used only here, outside the lock; the engine and pbufs only inside it.
    for (int n; (n = outerUdp_->parsePacket()) > 0;) {
        if (n > 0xFFFF) continue;   // the rest is dropped by the next parsePacket()
        lock();
        // PBUF_LINK: room in front, as the packet may be handed on (deliverInner()) and forwarded.
        struct pbuf *q = pbuf_alloc(PBUF_LINK, (u16_t)n, PBUF_RAM);
        unlock();
        if (!q) continue;
        uint8_t *data = static_cast<uint8_t *>(q->payload);
        int got = outerUdp_->read(data, n);
        IPAddress ip = outerUdp_->remoteIP();
        tinywg::Endpoint from = {{ip[0], ip[1], ip[2], ip[3]}, outerUdp_->remotePort()};
        lock();
        if (got == n) receive(q, from);
        else pbuf_free(q);
        unlock();
    }
    for (;;) {
        lock();
        if (!outgoingCount_) {
            unlock();
            break;
        }
        Outgoing o = outgoing_[outgoingHead_];
        outgoingHead_ = (outgoingHead_ + 1) % kOuterQueue;
        outgoingCount_--;
        unlock();
        outerUdp_->beginPacket(IPAddress(o.to.addr[0], o.to.addr[1], o.to.addr[2], o.to.addr[3]), o.to.port);
        outerUdp_->write(static_cast<const uint8_t *>(o.p->payload), o.p->len);
        outerUdp_->endPacket();
        lock();
        pbuf_free(o.p);
        unlock();
    }
}

#if defined(IP_NAPT) && IP_NAPT
bool TinyWG::enableNapt(bool enable)
{
    if (!up_) return false;
    lock();
    int ok = ip_napt_enable_netif(&netif_, enable ? 1 : 0);
    unlock();
    return ok != 0;
}
#endif

bool TinyWG::setMtu(uint16_t mtu)
{
    lock();
    bool ok = device_.setMtu(mtu);
    if (ok && up_) netif_.mtu = mtu;
    unlock();
    return ok;
}

err_t TinyWG::netifInit(struct netif *n)
{
    n->name[0] = 'w';
    n->name[1] = 'g';
    n->output = netifOutput;
    n->linkoutput = nullptr;
    n->mtu = static_cast<TinyWG *>(n->state)->device_.mtu();
    n->hwaddr_len = 0;
    n->flags = 0;   // point-to-point: no ARP, no broadcast
    return ERR_OK;
}

// lwIP routed an IPv4 packet to wg0.
err_t TinyWG::netifOutput(struct netif *n, struct pbuf *p, const ip4_addr_t *dest)
{
    (void)dest;
    TinyWG *self = static_cast<TinyWG *>(n->state);
    size_t len = p->tot_len;
    if (len > self->device_.mtu()) return ERR_BUF;
    const uint8_t *data;
    if (p->len == p->tot_len) {
        data = static_cast<const uint8_t *>(p->payload);
    } else {
        // A chain (header and data pbufs): gather it where the engine encrypts.
        uint8_t *buf = self->device_.innerBuffer();
        pbuf_copy_partial(p, buf, (u16_t)len, 0);
        data = buf;
    }
    switch (self->device_.sendInner(data, len, millis())) {
    case tinywg::SendResult::Sent:
    case tinywg::SendResult::Queued: return ERR_OK;
    case tinywg::SendResult::NoRoute: return ERR_RTE;
    default: return ERR_CONN;
    }
}

// An outer datagram on the listen port.
void TinyWG::udpReceive(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    TinyWG *self = static_cast<TinyWG *>(arg);
    if (!IP_IS_V4(addr)) {
        pbuf_free(p);
        return;
    }
    if (p->len != p->tot_len) {
        // Rare: a chain. Make it one contiguous pbuf, as the engine decrypts in place.
        struct pbuf *q = pbuf_clone(PBUF_LINK, PBUF_RAM, p);
        pbuf_free(p);
        p = q;
        if (!p) return;
    }
    const ip4_addr_t *a = ip_2_ip4(addr);
    self->receive(p, {{ip4_addr1(a), ip4_addr2(a), ip4_addr3(a), ip4_addr4(a)}, port});
}

// Decrypts the contiguous outer datagram p in place and frees it, unless
// deliverInner() handed it on to lwIP as the inner packet.
void TinyWG::receive(struct pbuf *p, const tinywg::Endpoint &from)
{
    // A pbuf over memory of its own can carry the inner packet on. Not one that
    // points into a driver's buffer (ESP32 Wi-Fi): lwIP could not put a link
    // header in front to forward it, and the driver wants its buffer back.
    bool own = p->next == nullptr && p->ref == 1 && (p->type_internal & PBUF_TYPE_FLAG_STRUCT_DATA_CONTIGUOUS);
    rxPbuf_ = own ? p : nullptr;
    device_.receiveOuter(static_cast<uint8_t *>(p->payload), p->tot_len, from, millis());
    if (rxPbuf_ || !own) pbuf_free(p);
    rxPbuf_ = nullptr;
}

void TinyWG::timer(void *arg)
{
    TinyWG *self = static_cast<TinyWG *>(arg);
    self->device_.tick(millis());
    sys_timeout(kTimerMs, timer, self);
}

void TinyWG::sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len)
{
    if (outerUdp_) {
        // Called with the lock held, from any context: pollOuter() sends it.
        if (outgoingCount_ == kOuterQueue) return;
        struct pbuf *q = pbuf_alloc(PBUF_RAW, (u16_t)len, PBUF_RAM);
        if (!q) return;
        memcpy(q->payload, data, len);
        outgoing_[(outgoingHead_ + outgoingCount_++) % kOuterQueue] = {q, to};
        return;
    }
    ip4_addr_t dest;
    IP4_ADDR(&dest, to.addr[0], to.addr[1], to.addr[2], to.addr[3]);
    // An endpoint routed into the tunnel itself would loop back here.
    if (ip4_route(&dest) == &netif_) return;
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!q) return;
    pbuf_take(q, data, (u16_t)len);
    ip_addr_t addr;
    ip_addr_copy_from_ip4(addr, dest);
    udp_sendto(pcb_, q, &addr, to.port);
    pbuf_free(q);
}

void TinyWG::deliverInner(int peer, uint8_t *packet, size_t len)
{
    (void)peer;
    struct pbuf *q = rxPbuf_;
    uint8_t *payload = q ? static_cast<uint8_t *>(q->payload) : nullptr;
    if (q && packet >= payload && packet + len <= payload + q->len) {
        // No copy: the outer pbuf, trimmed to the inner packet. The headers
        // removed in front leave room for a link header.
        rxPbuf_ = nullptr;
        pbuf_remove_header(q, (size_t)(packet - payload));
        pbuf_realloc(q, (u16_t)len);
        if (netif_.input(q, &netif_) != ERR_OK) pbuf_free(q);
        return;
    }
    // Room for a link header in front: a packet that lwIP forwards (NAPT) goes
    // out on Wi-Fi or Ethernet as it is, and cannot grow a header otherwise.
    q = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
    if (!q) return;
    pbuf_take(q, packet, (u16_t)len);
    if (netif_.input(q, &netif_) != ERR_OK) pbuf_free(q);
}

#endif
