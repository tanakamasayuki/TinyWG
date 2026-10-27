// On-board test: TinyWG as an lwIP interface (wg0 = 10.9.0.2/24) on Wi-Fi,
// reached from the PC by the Python WireGuard peer (helpers/wg_peer.py).
// Inside the tunnel lwIP answers ping by itself, and a WiFiUDP socket on
// port 7 echoes datagrams: the whole path through lwIP's sockets is used.
//
// Wi-Fi comes from tests/.env (TEST_WIFI_SSID / TEST_WIFI_PASSWORD via build_config.toml).
// Serial commands (one line each):
//   CFG <private> <peer public> [<psk>]   start wg0 with these keys (repeatable)
//   CONNECT                               start a handshake from the board
//   STATS                                 print the drop counters

#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

static TinyWG wg;
static WiFiUDP echo;
static int peer = -1;
static uint8_t buf[1500];

static void configure(char *line)
{
    char *argv[4] = {nullptr};
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 4; tok = strtok(nullptr, " ")) argv[argc++] = tok;
    if (argc < 3) {
        Serial.println("DUT-ERROR usage");
        return;
    }
    uint32_t t = micros();
    if (!wg.begin(IPAddress(10, 9, 0, 2), IPAddress(255, 255, 255, 0), argv[1], 51820)) {
        Serial.println("DUT-ERROR begin");
        return;
    }
    peer = wg.addPeer(argv[2], argc > 3 ? argv[3] : nullptr);
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 1), 32);
    Serial.printf("DUT-SETUP %lu us\n", (unsigned long)(micros() - t));
    Serial.print("DUT-READY ip=");
    Serial.print(WiFi.localIP());
    Serial.printf(" port=51820 peer=%d\n", peer);
}

static void command(char *line)
{
    if (strncmp(line, "CFG ", 4) == 0) {
        configure(line);
    } else if (strcmp(line, "CONNECT") == 0) {
        Serial.printf("CONNECT %d\n", wg.connect(peer) ? 1 : 0);
    } else if (strcmp(line, "STATS") == 0) {
        tinywg::Stats s = wg.stats();
        tinywg::PeerStatus p;
        wg.peerStatus(peer, &p);
        Serial.printf("STATS init=%u done=%u resp=%u badmac=%u hsfail=%u unknown=%u replay=%u decrypt=%u "
                      "notallowed=%u up=%d rx=%llu tx=%llu heap=%u\n",
                      (unsigned)s.handshakesInitiated, (unsigned)s.handshakesCompleted,
                      (unsigned)s.handshakesResponded, (unsigned)s.rxBadMac1, (unsigned)s.rxHandshakeFail,
                      (unsigned)s.rxUnknownPeer, (unsigned)s.rxReplay, (unsigned)s.rxDecryptFail,
                      (unsigned)s.rxNotAllowed, p.sessionUp ? 1 : 0, (unsigned long long)p.rxBytes,
                      (unsigned long long)p.txBytes, (unsigned)ESP.getFreeHeap());
    }
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("WIFI_ERROR missing_ssid (set TEST_WIFI_SSID in tests/.env)");
        return;
    }
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        Serial.printf("WIFI_DISCONNECT reason=%d\n", (int)info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (uint32_t start = millis(); WiFi.status() != WL_CONNECTED && millis() - start < 120000;) delay(250);
    if (WiFi.status() != WL_CONNECTED) {
        // Say whether the AP is visible at all (ESP32 sees 2.4 GHz only).
        WiFi.disconnect();
        delay(200);
        int n = WiFi.scanNetworks();
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == WIFI_SSID) {
                Serial.printf("WIFI_SCAN found rssi=%d channel=%d auth=%d\n", (int)WiFi.RSSI(i), (int)WiFi.channel(i), (int)WiFi.encryptionType(i));
            }
        }
        Serial.printf("WIFI_ERROR connect_failed status=%d visible_networks=%d\n", (int)WiFi.status(), n);
        return;
    }
    Serial.print("WIFI_OK ");
    Serial.println(WiFi.localIP());
    echo.begin(7);
    Serial.println("DUT-WAIT-CONFIG");
}

void loop()
{
    static char line[200];
    static size_t len = 0;
    while (Serial.available()) {
        int c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            line[len] = '\0';
            if (len) command(line);
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
    for (int n; (n = echo.parsePacket()) > 0;) {
        int got = echo.read(buf, sizeof(buf));
        echo.beginPacket(echo.remoteIP(), echo.remotePort());
        echo.write(buf, got);
        echo.endPacket();
    }
    delay(1);
}
