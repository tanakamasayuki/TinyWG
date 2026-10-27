// en: A WireGuard server for two peers, with a TCP echo server reachable only through the tunnel.
// ja: 2 ピア向けの WireGuard サーバー。トンネル越しにだけ届く TCP エコーサーバー付き。
#include <WiFi.h>
#include <TinyWG.h>

static const char *kWifiSsid = "your-ssid";
static const char *kWifiPass = "your-passphrase";

// en: [Interface] of this server. The peers use its public key and <LAN IP>:51820 as Endpoint.
// ja: このサーバーの [Interface]。ピア側はその公開鍵と <LAN の IP>:51820 を Endpoint にする。
static const char *kPrivateKey = "<this device private key>";
static const IPAddress kTunnelIp(10, 9, 0, 1);
static const IPAddress kTunnelMask(255, 255, 255, 0);
static constexpr uint16_t kListenPort = 51820;

// en: [Peer] entries: public key and tunnel address. No Endpoint: they connect to us.
// ja: [Peer]: 公開鍵とトンネル内アドレス。Endpoint は不要(相手から接続してくる)。
struct PeerConfig {
    const char *publicKey;
    IPAddress tunnelIp;
};
static const PeerConfig kPeers[] = {
    {"<peer public key>", IPAddress(10, 9, 0, 2)},
    {"<second peer public key>", IPAddress(10, 9, 0, 3)},
};

TinyWG wg;
WiFiServer echo(7);

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("TinyWG Esp32Server");

    WiFi.mode(WIFI_STA);
    WiFi.begin(kWifiSsid, kWifiPass);
    while (WiFi.status() != WL_CONNECTED) {
        delay(200);
        Serial.print('.');
    }
    Serial.print("\nEndpoint: ");
    Serial.print(WiFi.localIP());
    Serial.print(':');
    Serial.println(kListenPort);

    if (!wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, kListenPort)) {
        Serial.println("wg.begin() failed");
        return;
    }
    for (const PeerConfig &p : kPeers) {
        int index = wg.addPeer(p.publicKey);
        wg.addAllowedIp(index, p.tunnelIp, 32);
    }
    char pub[45];
    wg.publicKey(pub);
    Serial.print("Public key: ");
    Serial.println(pub);

    echo.begin();
}

void loop()
{
    WiFiClient client = echo.accept();
    if (client) {
        Serial.print("client ");
        Serial.println(client.remoteIP());
        while (client.connected()) {
            while (client.available()) client.write(client.read());
            delay(1);
        }
        client.stop();
    }

    static uint32_t last = 0;
    if (millis() - last >= 10000) {
        last = millis();
        for (size_t i = 0; i < sizeof(kPeers) / sizeof(kPeers[0]); i++) {
            tinywg::PeerStatus s;
            if (!wg.peerStatus((int)i, &s)) continue;
            Serial.printf("peer %u: %s rx=%llu tx=%llu\n", (unsigned)i, s.sessionUp ? "up" : "down",
                          (unsigned long long)s.rxBytes, (unsigned long long)s.txBytes);
        }
    }
}
