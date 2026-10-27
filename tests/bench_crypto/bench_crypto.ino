// Phase 0 measurements (SPEC.ja.md §7): what the protocol costs on this core.
// Prints "BENCH <name> <value> <unit>" lines, then "BENCH done".
//
//   x25519_*          one scalar multiplication
//   hs_*              the three handshake steps, end to end through tinywg::Device
//   aead_*, blake2s_* throughput of the data path and the hash
//   sizeof_*          RAM taken by the engine (static, no heap)

#include <TinyWG.h>
#include <tinywg/blake2s.h>
#include <tinywg/wg_crypto.h>
#include <string.h>

using namespace tinywg;

static void report(const char *name, double value, const char *unit)
{
    Serial.print("BENCH ");
    Serial.print(name);
    Serial.print(' ');
    Serial.print(value, 2);
    Serial.print(' ');
    Serial.println(unit);
}

// Captures the last outer datagram instead of sending it.
class Capture : public Transport {
public:
    size_t len = 0;
    uint8_t data[TINYWG_MTU + 64];
    void sendOuter(const Endpoint &, const uint8_t *d, size_t n) override
    {
        len = n;
        memcpy(data, d, n);
    }
    void deliverInner(int, uint8_t *, size_t) override {}
};

static Device g_a, g_b;
static Capture g_ca, g_cb;
static uint8_t g_buf[TINYWG_MTU + 16];

void setup()
{
    Serial.begin(115200);
    delay(5000);
    const int rounds = 4;

    uint8_t priv[32], peerPub[32], out[32];
    generatePrivateKey(priv);
    generatePrivateKey(out);
    derivePublicKey(peerPub, out);

    uint32_t t = micros();
    for (int i = 0; i < rounds; i++) crypto::publicKey(out, priv);
    report("x25519_base", (micros() - t) / 1000.0 / rounds, "ms");
    t = micros();
    for (int i = 0; i < rounds; i++) crypto::dh(out, priv, peerPub);
    report("x25519_dh", (micros() - t) / 1000.0 / rounds, "ms");

    // Handshake through the engine: A initiates, B responds, A completes.
    uint8_t privA[32], privB[32], pubA[32], pubB[32];
    generatePrivateKey(privA);
    generatePrivateKey(privB);
    derivePublicKey(pubA, privA);
    derivePublicKey(pubB, privB);
    g_a.begin(privA, &g_ca);
    g_b.begin(privB, &g_cb);
    t = micros();
    g_a.addPeer(pubB);
    report("add_peer", (micros() - t) / 1000.0, "ms");
    g_b.addPeer(pubA);
    const Endpoint epA = {{127, 0, 0, 1}, 1}, epB = {{127, 0, 0, 1}, 2};
    g_a.setEndpoint(0, epB);
    uint32_t now = 1000;
    double init = 0, resp = 0, done = 0;
    for (int i = 0; i < rounds; i++) {
        now += 6000;   // past the rate limit between initiations
        t = micros();
        g_a.connect(0, now);
        init += (micros() - t) / 1000.0;
        t = micros();
        g_b.receiveOuter(g_ca.data, g_ca.len, epA, now);
        resp += (micros() - t) / 1000.0;
        t = micros();
        g_a.receiveOuter(g_cb.data, g_cb.len, epB, now);
        done += (micros() - t) / 1000.0;
    }
    report("hs_initiation", init / rounds, "ms");
    report("hs_response", resp / rounds, "ms");
    report("hs_complete", done / rounds, "ms");
    report("hs_completed", g_a.stats().handshakesCompleted, "count");

    // Data path: ChaCha20-Poly1305 over a full-MTU packet, and BLAKE2s.
    uint8_t key[32];
    memset(key, 0x42, sizeof(key));
    memset(g_buf, 0x5A, sizeof(g_buf));
    const int packets = 64;
    t = micros();
    for (int i = 0; i < packets; i++) crypto::aeadEncrypt(g_buf, key, (uint64_t)i, g_buf, TINYWG_MTU, nullptr, 0);
    uint32_t us = micros() - t;
    report("aead_mtu", (double)TINYWG_MTU * packets / (us / 1e6) / 1024.0, "KiB/s");
    report("aead_packet", us / 1000.0 / packets, "ms");
    t = micros();
    for (int i = 0; i < packets; i++) tinywg_blake2s(out, 32, nullptr, 0, g_buf, TINYWG_MTU);
    us = micros() - t;
    report("blake2s", (double)TINYWG_MTU * packets / (us / 1e6) / 1024.0, "KiB/s");

    report("sizeof_device", sizeof(Device), "bytes");
    report("sizeof_peer", sizeof(detail::Peer), "bytes");
    report("max_peers", TINYWG_MAX_PEERS, "count");
    report("mtu", TINYWG_MTU, "bytes");
    Serial.println("BENCH done");
}

void loop()
{
    delay(1);
}
