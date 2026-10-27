#include "TinyWGLwip.h"

#ifdef TINYWG_HAS_LWIP

#include <lwip/ip4.h>
#include <lwip/timeouts.h>
#include <string.h>

#if defined(ARDUINO_ARCH_RP2040)
#if defined(__FREERTOS)
#error "TinyWG: arduino-pico in FreeRTOS mode is not supported yet (lwIP runs in its own task there)"
#endif
#include <lwip_wrap.h>
#include <new>
#elif !NO_SYS && LWIP_TCPIP_CORE_LOCKING
#include <lwip/tcpip.h>
#endif
#if defined(IP_NAPT) && IP_NAPT
#include <lwip/lwip_napt.h>
#endif

static constexpr uint32_t kTimerMs = 100;

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

bool TinyWG::begin(const IPAddress &localIp, const IPAddress &subnet, const uint8_t privateKey[32],
                   uint16_t listenPort)
{
    end();
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
    if (ok) {
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
    if (ok) {
        udp_recv(pcb_, udpReceive, this);
        sys_timeout(kTimerMs, timer, this);
        up_ = true;
    } else {
        device_.end();
    }
    unlock();
    return ok;
}

void TinyWG::end()
{
    lock();
    if (up_) {
        sys_untimeout(timer, this);
        udp_remove(pcb_);
        pcb_ = nullptr;
        netif_remove(&netif_);
        up_ = false;
    }
    device_.end();
    unlock();
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

err_t TinyWG::netifInit(struct netif *n)
{
    n->name[0] = 'w';
    n->name[1] = 'g';
    n->output = netifOutput;
    n->linkoutput = nullptr;
    n->mtu = TINYWG_MTU;
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
    if (len > TINYWG_MTU) return ERR_BUF;
    const uint8_t *data;
    if (p->len == p->tot_len) {
        data = static_cast<const uint8_t *>(p->payload);
    } else {
        pbuf_copy_partial(p, self->scratch_, (u16_t)len, 0);
        data = self->scratch_;
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
    size_t len = p->tot_len;
    if (IP_IS_V4(addr) && len <= sizeof(self->scratch_)) {
        const ip4_addr_t *a = ip_2_ip4(addr);
        tinywg::Endpoint from = {{ip4_addr1(a), ip4_addr2(a), ip4_addr3(a), ip4_addr4(a)}, port};
        uint8_t *data;
        if (p->len == p->tot_len) {
            data = static_cast<uint8_t *>(p->payload);   // decrypted in place
        } else {
            pbuf_copy_partial(p, self->scratch_, (u16_t)len, 0);
            data = self->scratch_;
        }
        self->device_.receiveOuter(data, len, from, millis());
    }
    pbuf_free(p);
}

void TinyWG::timer(void *arg)
{
    TinyWG *self = static_cast<TinyWG *>(arg);
    self->device_.tick(millis());
    sys_timeout(kTimerMs, timer, self);
}

void TinyWG::sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len)
{
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
    // Room for a link header in front: a packet that lwIP forwards (NAPT) goes
    // out on Wi-Fi or Ethernet as it is, and cannot grow a header otherwise.
    struct pbuf *q = pbuf_alloc(PBUF_LINK, (u16_t)len, PBUF_RAM);
    if (!q) return;
    pbuf_take(q, packet, (u16_t)len);
    if (netif_.input(q, &netif_) != ERR_OK) pbuf_free(q);
}

#endif
