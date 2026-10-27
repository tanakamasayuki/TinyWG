// en: Connect to a WireGuard server and send a UDP message through the tunnel every 5 s.
// ja: WireGuard サーバーに接続し、トンネル越しに 5 秒ごとに UDP を送る。
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>

static const char *kWifiSsid = "your-ssid";
static const char *kWifiPass = "your-passphrase";

// en: [Interface] of this device. Generate with `wg genkey`; give `wg pubkey` of it to the server.
// ja: このデバイスの [Interface]。`wg genkey` で作り、その `wg pubkey` をサーバー側に登録する。
static const char *kPrivateKey = "<this device private key>";
static const IPAddress kTunnelIp(10, 9, 0, 2);
static const IPAddress kTunnelMask(255, 255, 255, 0);

// en: [Peer] = the server.
// ja: [Peer] = サーバー。
static const char *kServerPublicKey = "<peer public key>";
static const IPAddress kServerEndpoint(192, 168, 1, 10);
static constexpr uint16_t kServerPort = 51820;
static const IPAddress kServerTunnelIp(10, 9, 0, 1);

TinyWG wg;
WiFiUDP udp;
int server = -1;

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("TinyWG Esp32Client");

    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPass);
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print('.');
    }
    Serial.print("\nLocal IP: ");
    Serial.println(WiFi.localIP());

    // en: The handshake timestamp must not go back across reboots: set the clock first.
    // ja: ハンドシェイクのタイムスタンプは再起動をまたいで戻れないので、先に時刻を合わせる。
    configTime(0, 0, "pool.ntp.org");
    struct tm now;
    getLocalTime(&now, 10000);

    if (!wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, 0)) {
        Serial.println("wg.begin() failed");
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
