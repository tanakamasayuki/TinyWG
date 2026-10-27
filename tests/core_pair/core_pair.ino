// Two tinywg::Device engines wired back to back in one process, with a
// simulated clock: handshake, data both ways, replay and tamper rejection,
// AllowedIPs enforcement, rekey, passive keepalive, key zeroing and
// handshake retransmission. Interop with a real WireGuard is kernel_interop/.

#include <TinyWG.h>
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

static Datagram g_wire[8];
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
        if (g_wireCount >= 8 || len > sizeof(g_wire[0].data)) return;
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
    static Datagram batch[8];
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
    Datagram copy = g_wire[0];
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

    // Out-of-order delivery inside the window is accepted.
    for (int i = 0; i < 3; i++) g_dev[0].sendInner(pkt, n1, g_now);
    Datagram ooo[3];
    memcpy(ooo, g_wire, sizeof(ooo));
    dropWire();
    int before = g_tr[1].inbox.count;
    for (int i = 2; i >= 0; i--) g_dev[1].receiveOuter(ooo[i].data, ooo[i].len, ooo[i].from, g_now);
    EXPECT_TRUE("out of order accepted", g_tr[1].inbox.count == before + 3);

    // Passive keepalive: B received data and stayed silent for 10 s.
    advance(9000);
    g_now += 1001;
    g_dev[1].tick(g_now);
    EXPECT_TRUE("passive keepalive", g_wireCount == 1 && g_wire[0].len == 32);
    pump();

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

    Serial.print("TEST done ");
    Serial.print(g_pass);
    Serial.print('/');
    Serial.println(g_total);
}

void loop()
{
    delay(1);
}
