// en: A WireGuard server for two peers, with a TCP echo server reachable only through the tunnel.
// ja: 2 ピア向けの WireGuard サーバー。トンネル越しにだけ届く TCP エコーサーバー付き。
#include <WiFi.h>
#include <WiFiUdp.h>
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

// en: Clients with no clock (no NTP, no RTC) may connect this long after they last talked; 0 = never.
// ja: 時計のないクライアント(NTP も RTC もない)を、最後の通信からこの秒数で受け付ける。0 = 受け付けない。
static constexpr uint16_t kTimestampReset = 30;
// en: Answer NTP on <LAN IP>:123 with this server's clock, so clients can use this address for NTP.
// ja: <LAN の IP>:123 で NTP に答え、このサーバーの時刻を配る。クライアントは NTP にこのアドレスを使える。
static constexpr bool kTimeServer = true;

TinyWG wg;
// en: With several peers, a packet waiting for one peer's handshake must not be lost when another is sent to.
// ja: ピアが複数あるので、あるピアのハンドシェイクを待つパケットが、ほかへの送信で消えないようにする。
static uint8_t staging[1420];
WiFiServer echo(7);
WiFiUDP ntpUdp;
TinyWGTimeServer timeServer;

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

    // en: Real time when the Internet is reachable; otherwise the clock counts from 1970 at each boot.
    // ja: インターネットにつながれば実時刻になる。つながらなければ、起動のたびに 1970 年から数える。
    configTime(0, 0, "pool.ntp.org");

    if (!wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, kListenPort)) {
        Serial.println("wg.begin() failed");
        return;
    }
    for (const PeerConfig &p : kPeers) {
        int index = wg.addPeer(p.publicKey);
        wg.addAllowedIp(index, p.tunnelIp, 32);
    }
    wg.setTimestampReset(kTimestampReset);
    wg.setStagingBuffer(staging, sizeof(staging));
    if (kTimeServer) timeServer.begin(ntpUdp);
    char pub[45];
    wg.publicKey(pub);
    Serial.print("Public key: ");
    Serial.println(pub);

    echo.begin();
}

void loop()
{
    timeServer.update();
    WiFiClient client = echo.accept();
    if (client) {
        Serial.print("client ");
        Serial.println(client.remoteIP());
        while (client.connected()) {
            while (client.available()) client.write(client.read());
            timeServer.update();
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
