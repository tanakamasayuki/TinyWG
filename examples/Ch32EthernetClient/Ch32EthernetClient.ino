// en: CH32V with Ethernet: connect to a WireGuard server and send a UDP message through the tunnel every 5 s.
// ja: Ethernet 付き CH32V: WireGuard サーバーに接続し、トンネル越しに 5 秒ごとに UDP を送る。

/* requires: ETH_MAC */

#include <Network.h>
#include <TinyWG.h>

// en: [Interface] of this device. Generate with `wg genkey`; give `wg pubkey` of it to the server.
// ja: このデバイスの [Interface]。`wg genkey` で作り、その `wg pubkey` をサーバー側に登録する。
static const char *kPrivateKey = "<this device private key>";
static const IPAddress kTunnelIp(10, 9, 0, 3);
static const IPAddress kTunnelMask(255, 255, 255, 0);

// en: [Peer] = the server.
// ja: [Peer] = サーバー。
static const char *kServerPublicKey = "<peer public key>";
static const IPAddress kServerEndpoint(192, 168, 1, 10);
static constexpr uint16_t kServerPort = 51820;
static const IPAddress kServerTunnelIp(10, 9, 0, 1);

static TinyWG wg;
static NetworkUDP udp;
static int server = -1;

void setup()
{
    Serial.begin(115200);
    delay(100);
    Serial.println("TinyWG Ch32EthernetClient");
    if (!ETH.begin()) {
        Serial.println("ETH.begin() failed");
        return;
    }
    while (!ETH.hasIP()) {
        delay(100);
    }
    Serial.print("Local IP: ");
    Serial.println(ETH.localIP());

    // en: The handshake timestamp must not go back across reboots: set the clock first.
    // ja: ハンドシェイクのタイムスタンプは再起動をまたいで戻れないので、先に時刻を合わせる。
    configTime(0, 0, "pool.ntp.org");
    for (uint32_t start = millis(); time(nullptr) < 1700000000 && millis() - start < 10000;) {
        delay(100);
    }

    if (!wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, 0)) {
        Serial.println("wg.begin() failed (key, or no random number generator on this series)");
        return;
    }
    server = wg.addPeer(kServerPublicKey);
    wg.addAllowedIp(server, IPAddress(10, 9, 0, 0), 24);
    wg.setEndpoint(server, kServerEndpoint, kServerPort);
    wg.setPersistentKeepalive(server, 25);
    wg.connect(server);

    char pub[45];
    wg.publicKey(pub);
    Serial.print("Public key: ");
    Serial.println(pub);
    udp.begin(0);
}

void loop()
{
    static uint32_t last = 0;
    if (server < 0 || millis() - last < 5000) return;
    last = millis();

    Serial.print("peer up: ");
    Serial.println(wg.peerUp(server) ? "yes" : "no");
    udp.beginPacket(kServerTunnelIp, 9000);
    udp.print("hello from TinyWG");
    udp.endPacket();
}
