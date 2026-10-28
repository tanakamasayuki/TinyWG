// Two tinywg::Device engines wired back to back in one process, with a
// simulated clock: handshake, data both ways, replay and tamper rejection,
// AllowedIPs enforcement, rekey, passive keepalive, key zeroing,
// handshake retransmission, cookie replies both ways, and the timestamp reset
// for a rebooted peer.
// Interop with a real WireGuard is kernel_interop/.

#include <TinyWG.h>
#include <tinywg/wg_crypto.h>
#include <string.h>

using namespace tinywg;

static int g_pass = 0;
static int g_total = 0;

#define EXPECT_TRUE(name, cond)      \
    do                               \
    {                                \
        ++g_total;                   \
        if (cond)                    \
        {                            \
            ++g_pass;                \
            Serial.print("PASS ");   \
            Serial.println(name);    \
        }                            \
        else                         \
        {                            \
            Serial.print("FAIL ");   \
            Serial.print(name);      \
            Serial.println(" cond"); \
        }                            \
    } while (0)

// ---- a tiny wire: outer datagrams queue here until pump() delivers them

struct Datagram {
    int to;
    Endpoint from;
    size_t len;
    uint8_t data[TINYWG_MTU + 64];
};

static constexpr int kWireSlots = 4;
static Datagram g_wire[kWireSlots];
static int g_wireCount;
static uint32_t g_now = 1000;

struct Inbox {
    int count;
    size_t lastLen;
    uint8_t last[TINYWG_MTU];
};

class Wire : public Transport {
public:
    int self;
    Inbox inbox;
    void sendOuter(const Endpoint &to, const uint8_t *data, size_t len) override
    {
        if (g_wireCount >= kWireSlots || len > sizeof(g_wire[0].data)) return;
        Datagram &d = g_wire[g_wireCount++];
        d.to = to.port == 1 ? 0 : 1;
        d.from = Endpoint{{127, 0, 0, 1}, (uint16_t)(self + 1)};
        d.len = len;
        memcpy(d.data, data, len);
    }
    void deliverInner(int, uint8_t *packet, size_t len) override
    {
        inbox.count++;
        inbox.lastLen = len;
        memcpy(inbox.last, packet, len);
    }
};

static Device g_dev[2];
static Wire g_tr[2];

static void dropWire() { g_wireCount = 0; }

// Delivers until the wire is quiet; returns the number of datagrams delivered.
static int pump()
{
    int delivered = 0;
    static Datagram batch[kWireSlots];
    while (g_wireCount > 0) {
        int n = g_wireCount;
        memcpy(batch, g_wire, sizeof(Datagram) * n);
        g_wireCount = 0;
        for (int i = 0; i < n; i++) {
            g_dev[batch[i].to].receiveOuter(batch[i].data, batch[i].len, batch[i].from, g_now);
            delivered++;
        }
    }
    return delivered;
}

static void advance(uint32_t ms, uint32_t step = 1000)
{
    for (uint32_t t = 0; t < ms; t += step) {
        g_now += step;
        g_dev[0].tick(g_now);
        g_dev[1].tick(g_now);
        pump();
    }
}

static size_t makeIp(uint8_t *p, const uint8_t src[4], const uint8_t dst[4], size_t payload)
{
    size_t n = 20 + payload;
    memset(p, 0, n);
    p[0] = 0x45;
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
    memcpy(p + 12, src, 4);
    memcpy(p + 16, dst, 4);
    for (size_t i = 0; i < payload; i++) p[20 + i] = (uint8_t)i;
    return n;
}

// A packet for a new peer waits for its handshake while A sends to B, which
// has a session. Returns the length of what A sends the new peer once the
// handshake completes: 32 is a bare keepalive (the waiting packet was lost).
static size_t stagedThenOtherSend(Device &other, int index, const uint8_t ipSelf[4], const uint8_t ipOther[4],
                                  const uint8_t *toB, size_t toBLen)
{
    static uint8_t p[TINYWG_MTU];
    dropWire();
    size_t n = makeIp(p, ipSelf, ipOther, 50);
    if (g_dev[0].sendInner(p, n, g_now) != SendResult::Queued) return 0;
    static Datagram init;
    init = g_wire[0];   // the initiation to the new peer
    dropWire();
    if (g_dev[0].sendInner(toB, toBLen, g_now) != SendResult::Sent) return 0;
    dropWire();
    other.receiveOuter(init.data, init.len, Endpoint{{127, 0, 0, 1}, 1}, g_now);
    static Datagram resp;
    resp = g_wire[0];
    dropWire();
    g_dev[0].receiveOuter(resp.data, resp.len, Endpoint{{127, 0, 0, 1}, (uint16_t)(3 + index)}, g_now);
    size_t len = g_wireCount ? g_wire[0].len : 0;
    dropWire();
    return len;
}

static bool sessionUp(int dev)
{
    PeerStatus s;
    return g_dev[dev].peerStatus(0, &s) && s.sessionUp;
}

void setup()
{
    Serial.begin(115200);
    delay(5000);

    static const uint8_t ipA[4] = {10, 0, 0, 1}, ipB[4] = {10, 0, 0, 2}, ipX[4] = {10, 0, 0, 9};
    static uint8_t pkt[TINYWG_MTU], pkt2[TINYWG_MTU];
    uint8_t priv[2][32], pub[2][32], psk[32];
    for (int i = 0; i < 2; i++) {
        generatePrivateKey(priv[i]);
        derivePublicKey(pub[i], priv[i]);
        g_tr[i].self = i;
        EXPECT_TRUE("begin", g_dev[i].begin(priv[i], &g_tr[i]));
    }
    EXPECT_TRUE("public key", memcmp(g_dev[0].publicKey(), pub[0], 32) == 0);
    generatePrivateKey(psk);
    EXPECT_TRUE("add peer A", g_dev[0].addPeer(pub[1], psk) == 0);
    EXPECT_TRUE("add peer B", g_dev[1].addPeer(pub[0], psk) == 0);
    EXPECT_TRUE("duplicate peer rejected", g_dev[0].addPeer(pub[1]) == -1);
    g_dev[0].addAllowedIp(0, ipB, 32);
    g_dev[1].addAllowedIp(0, ipA, 32);
    g_dev[0].setEndpoint(0, Endpoint{{127, 0, 0, 1}, 2});   // B has no endpoint: it learns A's

    // First packet: staged, handshake, delivered once the session is up.
    size_t n1 = makeIp(pkt, ipA, ipB, 100);
    EXPECT_TRUE("first send queued", g_dev[0].sendInner(pkt, n1, g_now) == SendResult::Queued);
    pump();
    EXPECT_TRUE("first packet delivered", g_tr[1].inbox.count == 1 && g_tr[1].inbox.lastLen == n1 &&
                                              memcmp(g_tr[1].inbox.last, pkt, n1) == 0);
    EXPECT_TRUE("initiator session up", sessionUp(0));
    EXPECT_TRUE("responder session up", sessionUp(1));

    // Reply from the responder, then replay it.
    size_t n2 = makeIp(pkt2, ipB, ipA, 33);
    EXPECT_TRUE("reply sent", g_dev[1].sendInner(pkt2, n2, g_now) == SendResult::Sent);
    static Datagram copy;   // static: a Datagram is ~1.5 KB, too big for an 8 KB loop stack
    copy = g_wire[0];
    pump();
    EXPECT_TRUE("reply delivered", g_tr[0].inbox.count == 1 && memcmp(g_tr[0].inbox.last, pkt2, n2) == 0);
    uint32_t replay0 = g_dev[0].stats().rxReplay;
    g_dev[0].receiveOuter(copy.data, copy.len, copy.from, g_now);
    EXPECT_TRUE("replay rejected", g_dev[0].stats().rxReplay == replay0 + 1 && g_tr[0].inbox.count == 1);

    // A tampered fresh packet fails authentication.
    g_dev[1].sendInner(pkt2, n2, g_now);
    copy = g_wire[0];
    dropWire();
    copy.data[20] ^= 1;
    g_dev[0].receiveOuter(copy.data, copy.len, copy.from, g_now);
    EXPECT_TRUE("tamper rejected", g_dev[0].stats().rxDecryptFail == 1 && g_tr[0].inbox.count == 1);

    // An inner source outside the peer's AllowedIPs is dropped.
    size_t nx = makeIp(pkt2, ipX, ipA, 10);
    g_dev[1].sendInner(pkt2, nx, g_now);
    pump();
    EXPECT_TRUE("spoofed source dropped", g_dev[0].stats().rxNotAllowed == 1 && g_tr[0].inbox.count == 1);
    size_t nr = makeIp(pkt2, ipA, ipX, 1);
    EXPECT_TRUE("no route", g_dev[0].sendInner(pkt2, nr, g_now) == SendResult::NoRoute);

    int before;
    // A smaller MTU at run time: the bounds, a packet over it, and no padding past it.
    EXPECT_TRUE("mtu bounds", !g_dev[0].setMtu(Device::kMinMtu - 1) && !g_dev[0].setMtu(TINYWG_MTU + 1) &&
                                  g_dev[0].mtu() == TINYWG_MTU);
    EXPECT_TRUE("mtu set", g_dev[0].setMtu(1000) && g_dev[0].mtu() == 1000);
    size_t big = makeIp(pkt2, ipA, ipB, 1001 - 20);
    EXPECT_TRUE("over mtu refused", g_dev[0].sendInner(pkt2, big, g_now) == SendResult::Invalid);
    size_t odd = makeIp(pkt2, ipA, ipB, 999 - 20);
    before = g_tr[1].inbox.count;
    EXPECT_TRUE("at mtu sent", g_dev[0].sendInner(pkt2, odd, g_now) == SendResult::Sent);
    EXPECT_TRUE("no padding past mtu", g_wireCount == 1 && g_wire[0].len == 16 + 999 + 16);
    pump();
    EXPECT_TRUE("at mtu delivered", g_tr[1].inbox.count == before + 1 && g_tr[1].inbox.lastLen == odd);
    g_dev[0].setMtu(TINYWG_MTU);

    // Out-of-order delivery inside the window is accepted.
    for (int i = 0; i < 3; i++) g_dev[0].sendInner(pkt, n1, g_now);
    static Datagram ooo[3];
    memcpy(ooo, g_wire, sizeof(ooo));
    dropWire();
    before = g_tr[1].inbox.count;
    for (int i = 2; i >= 0; i--) g_dev[1].receiveOuter(ooo[i].data, ooo[i].len, ooo[i].from, g_now);
    EXPECT_TRUE("out of order accepted", g_tr[1].inbox.count == before + 3);

    // Passive keepalive: B received data and stayed silent for 10 s.
    advance(9000);
    g_now += 1001;
    g_dev[1].tick(g_now);
    EXPECT_TRUE("passive keepalive", g_wireCount == 1 && g_wire[0].len == 32);
    pump();

    // Persistent keepalive turned on while idle: one at the next tick, then every interval.
    EXPECT_TRUE("set keepalive", g_dev[0].setPersistentKeepalive(0, 25));
    g_now += 100;
    g_dev[0].tick(g_now);
    EXPECT_TRUE("keepalive at once", g_wireCount == 1 && g_wire[0].len == 32);
    pump();
    g_now += 24000;
    g_dev[0].tick(g_now);
    EXPECT_TRUE("no keepalive before interval", g_wireCount == 0);
    g_now += 1000;
    g_dev[0].tick(g_now);
    EXPECT_TRUE("keepalive at interval", g_wireCount == 1 && g_wire[0].len == 32);
    pump();
    g_dev[0].setPersistentKeepalive(0, 0);
    g_now += 30000;
    g_dev[0].tick(g_now);
    EXPECT_TRUE("keepalive off", g_wireCount == 0);

    // Rekey: the initiator starts a new handshake on send after 120 s.
    uint32_t hs0 = g_dev[0].stats().handshakesInitiated;
    advance(120000);
    g_dev[0].sendInner(pkt, n1, g_now);
    pump();
    EXPECT_TRUE("rekey after time", g_dev[0].stats().handshakesInitiated > hs0);
    before = g_tr[1].inbox.count;
    EXPECT_TRUE("send on new session", g_dev[0].sendInner(pkt, n1, g_now) == SendResult::Sent);
    pump();
    EXPECT_TRUE("delivered on new session", g_tr[1].inbox.count == before + 1);

    // Silence: keys are zeroed after 3 x 180 s; new data brings the session back.
    advance(600000);
    EXPECT_TRUE("keys zeroed", !sessionUp(0) && !sessionUp(1));
    before = g_tr[1].inbox.count;
    EXPECT_TRUE("send after zeroing queued", g_dev[0].sendInner(pkt, n1, g_now) == SendResult::Queued);
    pump();
    EXPECT_TRUE("recovered after zeroing", g_tr[1].inbox.count == before + 1);

    // An unanswered initiation is retried every 5 s (+ jitter) for 90 s, then given up.
    {
        static Device lone;
        static Wire loneWire;
        loneWire.self = 0;
        lone.begin(priv[0], &loneWire);
        lone.addPeer(pub[1]);
        lone.addAllowedIp(0, ipB, 32);
        lone.setEndpoint(0, Endpoint{{127, 0, 0, 1}, 2});
        dropWire();
        lone.sendInner(pkt, n1, g_now);
        int sent = g_wireCount;
        dropWire();
        for (int i = 0; i < 1000; i++) {
            g_now += 100;
            lone.tick(g_now);
            sent += g_wireCount;
            dropWire();
        }
        Serial.print("initiations in 100 s: ");
        Serial.println(sent);
        EXPECT_TRUE("retransmit count", sent >= 17 && sent <= 19);
    }

    // A wrong preshared key never completes a handshake.
    {
        static Device c;
        static Wire cw;
        cw.self = 0;
        uint8_t wrong[32];
        generatePrivateKey(wrong);
        c.begin(priv[0], &cw);
        c.addPeer(pub[1], wrong);
        c.addAllowedIp(0, ipB, 32);
        c.setEndpoint(0, Endpoint{{127, 0, 0, 1}, 2});
        dropWire();
        uint32_t done = g_dev[1].stats().handshakesResponded;
        c.sendInner(pkt, n1, g_now);
        Datagram init = g_wire[0];
        dropWire();
        g_dev[1].receiveOuter(init.data, init.len, init.from, g_now);
        Datagram resp = g_wire[0];
        dropWire();
        EXPECT_TRUE("responder answers", g_dev[1].stats().handshakesResponded == done + 1);
        c.receiveOuter(resp.data, resp.len, resp.from, g_now);
        PeerStatus s;
        c.peerStatus(0, &s);
        EXPECT_TRUE("wrong psk rejected", !s.sessionUp && c.stats().rxHandshakeFail == 1);
    }

    // A cookie reply (a responder under load): the initiator opens it with the
    // responder's public key and sends mac2 = MAC(cookie, message) from then on.
    {
        static Device ci;
        static Wire ciw;
        ciw.self = 0;
        ci.begin(priv[0], &ciw);
        ci.addPeer(pub[1]);
        ci.addAllowedIp(0, ipB, 32);
        ci.setEndpoint(0, Endpoint{{127, 0, 0, 1}, 2});
        dropWire();
        ci.connect(0, g_now);
        static Datagram init;
        init = g_wire[0];
        dropWire();
        uint8_t zero[16] = {0};
        EXPECT_TRUE("mac2 zero", memcmp(init.data + 132, zero, 16) == 0);

        uint8_t cookie[16], key[32], reply[64] = {3};
        for (int i = 0; i < 16; i++) cookie[i] = (uint8_t)(0xC0 + i);
        memcpy(reply + 4, init.data + 4, 4);                         // receiver: the initiator's index
        for (int i = 0; i < 24; i++) reply[8 + i] = (uint8_t)i;      // nonce
        crypto::hash(key, (const uint8_t *)"cookie--", 8, pub[1], 32);
        crypto::xaeadEncrypt(reply + 32, key, reply + 8, cookie, 16, init.data + 116, 16);   // AD: its mac1
        ci.receiveOuter(reply, sizeof(reply), Endpoint{{127, 0, 0, 1}, 2}, g_now);
        EXPECT_TRUE("cookie accepted", ci.stats().rxHandshakeFail == 0);

        g_now += 6000;
        ci.tick(g_now);   // the retransmission carries mac2
        EXPECT_TRUE("initiation resent", g_wireCount == 1);
        uint8_t expect[16];
        crypto::mac(expect, cookie, 16, g_wire[0].data, 148 - 16);
        EXPECT_TRUE("mac2 from cookie", memcmp(g_wire[0].data + 132, expect, 16) == 0);
        dropWire();
        ci.end();
    }

    // A responder with a CookieGuard (1 handshake a second before it counts as
    // under load): a second initiation in the same second gets a cookie reply
    // instead of an answer; the initiator's retransmission carries mac2 and is
    // answered, but not when it comes from another source port.
    {
        static Device gr, i1, i2;
        static Wire grw, i1w, i2w;
        static tinywg::CookieGuard guard(1);
        uint8_t privG[32], pubG[32], priv1[32], pub1[32], priv2[32], pub2[32];
        generatePrivateKey(privG);
        derivePublicKey(pubG, privG);
        generatePrivateKey(priv1);
        derivePublicKey(pub1, priv1);
        generatePrivateKey(priv2);
        derivePublicKey(pub2, priv2);
        grw.self = 1;
        i1w.self = 0;
        i2w.self = 0;
        gr.begin(privG, &grw);
        gr.setHandshakeGuard(&guard);
        gr.addPeer(pub1);
        gr.addPeer(pub2);
        const Endpoint grEnd{{127, 0, 0, 1}, 2}, from1{{127, 0, 0, 1}, 101}, from2{{127, 0, 0, 1}, 102};
        i1.begin(priv1, &i1w);
        i1.addPeer(pubG);
        i1.setEndpoint(0, grEnd);
        i2.begin(priv2, &i2w);
        i2.addPeer(pubG);
        i2.setEndpoint(0, grEnd);
        dropWire();

        i1.connect(0, g_now);
        static Datagram m, init1;
        init1 = m = g_wire[0];
        dropWire();
        gr.receiveOuter(m.data, m.len, from1, g_now);
        EXPECT_TRUE("guard: first answered", gr.stats().handshakesResponded == 1 && g_wireCount == 1 && g_wire[0].len == 92);
        dropWire();

        i2.connect(0, g_now);
        static Datagram init2;
        init2 = g_wire[0];
        dropWire();
        gr.receiveOuter(init2.data, init2.len, from2, g_now);
        EXPECT_TRUE("guard: cookie under load", gr.stats().cookiesSent == 1 && gr.stats().handshakesResponded == 1 &&
                                                    g_wireCount == 1 && g_wire[0].len == 64);
        m = g_wire[0];
        dropWire();
        i2.receiveOuter(m.data, m.len, grEnd, g_now);
        EXPECT_TRUE("guard: cookie taken", i2.stats().rxHandshakeFail == 0);

        g_now += 6000;
        i2.tick(g_now);   // retransmission with mac2
        static Datagram retry;
        retry = g_wire[0];
        dropWire();
        gr.receiveOuter(init1.data, init1.len, from1, g_now);   // a replay: counts toward the load, not answered
        dropWire();
        gr.receiveOuter(retry.data, retry.len, Endpoint{{127, 0, 0, 1}, 103}, g_now);
        EXPECT_TRUE("guard: mac2 tied to source", gr.stats().cookiesSent == 2 && gr.stats().handshakesResponded == 1);
        dropWire();
        gr.receiveOuter(retry.data, retry.len, from2, g_now);
        EXPECT_TRUE("guard: mac2 answered", gr.stats().handshakesResponded == 2 && g_wireCount == 1 && g_wire[0].len == 92);
        dropWire();
        gr.end();
        i1.end();
        i2.end();
    }

    // The packet waiting for a handshake sits in the send buffer by default and
    // is lost when A sends to another peer meanwhile; a staging buffer keeps it.
    {
        static Device c, d;
        static Wire cw, dw;
        static const uint8_t ipC[4] = {10, 0, 0, 3}, ipD[4] = {10, 0, 0, 4};
        uint8_t privC[32], pubC[32], privD[32], pubD[32];
        generatePrivateKey(privC);
        derivePublicKey(pubC, privC);
        generatePrivateKey(privD);
        derivePublicKey(pubD, privD);
        cw.self = 1;
        dw.self = 1;
        c.begin(privC, &cw);
        c.addPeer(pub[0]);
        c.addAllowedIp(0, ipA, 32);
        d.begin(privD, &dw);
        d.addPeer(pub[0]);
        d.addAllowedIp(0, ipA, 32);
        int pc = g_dev[0].addPeer(pubC), pd = g_dev[0].addPeer(pubD);
        g_dev[0].addAllowedIp(pc, ipC, 32);
        g_dev[0].setEndpoint(pc, Endpoint{{127, 0, 0, 1}, (uint16_t)(3 + pc)});
        g_dev[0].addAllowedIp(pd, ipD, 32);
        g_dev[0].setEndpoint(pd, Endpoint{{127, 0, 0, 1}, (uint16_t)(3 + pd)});

        g_dev[0].sendInner(pkt, n1, g_now);   // make sure A and B have a session
        pump();
        EXPECT_TRUE("staged lost by default", stagedThenOtherSend(c, pc, ipA, ipC, pkt, n1) == 32);
        static uint8_t staging[TINYWG_MTU];
        EXPECT_TRUE("staging buffer too small", !g_dev[0].setStagingBuffer(staging, Device::kMinMtu - 1));
        EXPECT_TRUE("staging buffer", g_dev[0].setStagingBuffer(staging, sizeof(staging)));
        EXPECT_TRUE("staged kept with a buffer", stagedThenOtherSend(d, pd, ipA, ipD, pkt, n1) == 16 + 80 + 16);
        g_dev[0].setStagingBuffer(nullptr, 0);
        g_dev[0].removePeer(pc);
        g_dev[0].removePeer(pd);
        c.end();
        d.end();
    }

    // A rebooted A whose clock starts at 1970 again: B refuses its older
    // timestamp while A's old session is active, and with setTimestampReset()
    // accepts it once A has been silent that long.
    {
        static Device r;
        static Wire rw;
        rw.self = 0;
        r.setWallClock([](uint64_t *s, uint32_t *ns) {
            *s = g_now / 1000;
            *ns = 0;
        });
        r.begin(priv[0], &rw);
        r.addPeer(pub[1], psk);
        r.addAllowedIp(0, ipB, 32);
        r.setEndpoint(0, Endpoint{{127, 0, 0, 1}, 2});
        g_dev[0].sendInner(pkt, n1, g_now);   // B hears from the old A just now
        pump();
        dropWire();

        uint32_t stale = g_dev[1].stats().rxStaleTimestamp;
        r.sendInner(pkt, n1, g_now);
        static Datagram oldInit;
        oldInit = g_wire[0];
        dropWire();
        g_dev[1].receiveOuter(oldInit.data, oldInit.len, oldInit.from, g_now);
        EXPECT_TRUE("older timestamp refused", g_dev[1].stats().rxStaleTimestamp == stale + 1 && g_wireCount == 0);

        g_dev[1].setTimestampReset(30);
        g_now += 29000;
        r.connect(0, g_now);
        Datagram init = g_wire[0];
        dropWire();
        g_dev[1].receiveOuter(init.data, init.len, init.from, g_now);
        EXPECT_TRUE("refused within reset time", g_dev[1].stats().rxStaleTimestamp == stale + 2 && g_wireCount == 0);

        g_now += 6000;
        r.connect(0, g_now);
        init = g_wire[0];
        dropWire();
        uint32_t responded = g_dev[1].stats().handshakesResponded;
        g_dev[1].receiveOuter(init.data, init.len, init.from, g_now);
        EXPECT_TRUE("accepted after reset time", g_dev[1].stats().handshakesResponded == responded + 1);
        Datagram resp = g_wire[0];
        dropWire();
        r.receiveOuter(resp.data, resp.len, resp.from, g_now);   // A confirms with a keepalive
        Datagram conf = g_wire[0];
        dropWire();
        g_dev[1].receiveOuter(conf.data, conf.len, conf.from, g_now);
        before = g_tr[1].inbox.count;
        r.sendInner(pkt, n1, g_now);
        Datagram data = g_wire[0];
        dropWire();
        g_dev[1].receiveOuter(data.data, data.len, data.from, g_now);
        EXPECT_TRUE("rebooted peer delivers", g_tr[1].inbox.count == before + 1);

        // Replaying the first, older initiation fails while the new session is active.
        g_now += 1000;
        g_dev[1].receiveOuter(oldInit.data, oldInit.len, oldInit.from, g_now);
        EXPECT_TRUE("replay refused while active", g_dev[1].stats().rxStaleTimestamp == stale + 3);
        g_dev[1].setTimestampReset(0);
    }

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print('/');
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
