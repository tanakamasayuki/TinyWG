// en: A WireGuard client over a W5500 driven by the Arduino Ethernet library (its own TCP/IP stack).
//     wg0 stays in the ESP32's lwIP; only the encrypted outer UDP goes through the W5500.
// ja: Arduino の Ethernet ライブラリ(モジュール側の TCP/IP)で動かす W5500 経由の WireGuard クライアント。
//     wg0 は ESP32 の lwIP に置いたまま、暗号化した外側の UDP だけを W5500 から送る。
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Network.h>
#include <NetworkUdp.h>
#include <TinyWG.h>
#include <sys/time.h>

// en: W5500 on the default SPI pins (SCK 18, MISO 19, MOSI 23) with chip select on this pin.
// ja: W5500 は標準の SPI ピン(SCK 18、MISO 19、MOSI 23)。チップセレクトはこのピン。
static constexpr int kCsPin = 5;
static uint8_t kMac[6] = {0x02, 0x00, 0x00, 0x57, 0x47, 0x01};   // locally administered

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

// en: NTP through the W5500. On a network with no Internet, use an NTP server inside it,
//     e.g. the WireGuard server's address when it runs TinyWGTimeServer.
// ja: NTP は W5500 経由。インターネットのない網では、網の中の NTP サーバーを使う
//     (WireGuard サーバーが TinyWGTimeServer を動かしていれば、そのアドレス)。
static const char *kNtpServer = "pool.ntp.org";

EthernetUDP outer;   // en: WireGuard's outer UDP / ja: WireGuard の外側の UDP
TinyWG wg;
NetworkUDP udp;      // en: inside the tunnel, on lwIP / ja: トンネルの中(lwIP)
int server = -1;

// en: One SNTP query over the W5500; sets the clock that the handshake timestamps use.
// ja: W5500 経由で SNTP を 1 回問い合わせ、ハンドシェイクの時刻に使う時計を合わせる。
static bool syncClock()
{
    EthernetUDP ntp;
    uint8_t packet[48] = {0x23};   // version 4, client mode
    if (!ntp.begin(12300)) return false;
    bool ok = false;
    if (ntp.beginPacket(kNtpServer, 123) && ntp.write(packet, sizeof(packet)) == sizeof(packet) && ntp.endPacket()) {
        for (uint32_t start = millis(); millis() - start < 3000; delay(10)) {
            if (ntp.parsePacket() < 48) continue;
            ntp.read(packet, sizeof(packet));
            uint32_t ntpSeconds = (uint32_t)packet[40] << 24 | (uint32_t)packet[41] << 16 | (uint32_t)packet[42] << 8 | packet[43];
            struct timeval tv = {(time_t)(ntpSeconds - 2208988800u), 0};
            settimeofday(&tv, nullptr);
            ok = true;
            break;
        }
    }
    ntp.stop();
    return ok;
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("TinyWG Esp32W5500Client");

    Ethernet.init(kCsPin);
    if (!Ethernet.begin(kMac)) {
        Serial.println(Ethernet.hardwareStatus() == EthernetNoHardware ? "W5500 not found" : "DHCP failed");
        return;
    }
    Serial.print("Local IP: ");
    Serial.println(Ethernet.localIP());

    // en: The handshake timestamp must not go back across reboots: set the clock first.
    // ja: ハンドシェイクのタイムスタンプは再起動をまたいで戻れないので、先に時刻を合わせる。
    Serial.println(syncClock() ? "clock set" : "clock not set");

    // en: Start lwIP without Wi-Fi: wg0 lives there.
    // ja: Wi-Fi なしで lwIP を起動する。wg0 はそこに置く。
    Network.begin();

    // en: true: loop() drives the W5500 through wg.update(), as it also calls Ethernet.maintain().
    //     Nothing in loop() may then block for long; see the README for the other mode.
    // ja: true: loop() の wg.update() で W5500 を回す。Ethernet.maintain() も loop() で呼ぶため。
    //     この場合 loop() を長くふさがないこと。もう一つの方式は README を参照。
    wg.setOuterUdp(&outer, true);
    if (!wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, 0)) {
        Serial.println("wg.begin() failed");
        return;
    }
    server = wg.addPeer(kServerPublicKey);
    wg.addAllowedIp(server, IPAddress(10, 9, 0, 0), 24);
    wg.setEndpoint(server, kServerEndpoint, kServerPort);
    wg.setPersistentKeepalive(server, 25);

    char pub[45];
    wg.publicKey(pub);
    Serial.print("Public key: ");
    Serial.println(pub);
    udp.begin(0);
}

void loop()
{
    wg.update();
    Ethernet.maintain();

    static uint32_t last = 0;
    if (server < 0 || millis() - last < 5000) return;
    last = millis();

    Serial.print("peer up: ");
    Serial.println(wg.peerUp(server) ? "yes" : "no");
    udp.beginPacket(kServerTunnelIp, 9000);
    udp.print("hello from TinyWG over W5500");
    udp.endPacket();
}
