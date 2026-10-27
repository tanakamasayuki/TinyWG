// On-board test: TinyWG as an lwIP interface (wg0 = 10.9.0.2/24) on Wi-Fi
// (ESP32) or Ethernet (CH32V307), reached from the PC by the Python
// WireGuard peer (helpers/wg_peer.py). Inside the tunnel lwIP answers ping
// by itself, and a UDP socket on port 7 echoes datagrams: the whole path
// through lwIP's sockets is used.
//
// Wi-Fi comes from tests/.env (TEST_WIFI_SSID / TEST_WIFI_PASSWORD via build_config.toml).
// The board prints NET_OK <ip> (or NET_ERROR <why>) once the network is up.
// Serial commands (one line each):
//   CFG <private> <peer public> [<psk>]   start wg0 with these keys (repeatable)
//   CONNECT                               start a handshake from the board
//   KEEPALIVE <seconds>                   persistent keepalive (0 = off)
//   MTU <bytes>                           wg0's MTU at run time
//   NAPT                                  route the peer to the LAN through NAPT (ESP32)
//   STATS                                 print the drop counters

#if defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#include <WiFiUdp.h>
typedef WiFiUDP EchoUDP;
#else
#include <Network.h>
typedef NetworkUDP EchoUDP;
#endif
#include <TinyWG.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#endif

static TinyWG wg;
static EchoUDP echo;
static int peer = -1;
static uint8_t buf[1500];

// Not every core has Serial.printf, and newlib-nano has no %llu.
static void out(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Serial.print(line);
}

static unsigned freeHeap()
{
#if defined(ARDUINO_ARCH_ESP32)
    return ESP.getFreeHeap();
#else
    return 0;
#endif
}

static IPAddress localIP()
{
#if defined(ARDUINO_ARCH_ESP32)
    return WiFi.localIP();
#else
    return ETH.localIP();
#endif
}

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
    out("DUT-SETUP %lu us\n", (unsigned long)(micros() - t));
    Serial.print("DUT-READY ip=");
    Serial.print(localIP());
    out(" port=51820 peer=%d\n", peer);
}

static void command(char *line)
{
    if (strncmp(line, "CFG ", 4) == 0) {
        configure(line);
    } else if (strcmp(line, "CONNECT") == 0) {
        out("CONNECT %d\n", wg.connect(peer) ? 1 : 0);
    } else if (strncmp(line, "KEEPALIVE ", 10) == 0) {
        out("KEEPALIVE %d\n", wg.setPersistentKeepalive(peer, (uint16_t)atoi(line + 10)) ? 1 : 0);
    } else if (strncmp(line, "MTU ", 4) == 0) {
        out("MTU %d %u\n", wg.setMtu((uint16_t)atoi(line + 4)) ? 1 : 0, (unsigned)wg.mtu());
    } else if (strcmp(line, "NAPT") == 0) {
#if defined(IP_NAPT) && IP_NAPT
        out("NAPT %d\n", wg.enableNapt() ? 1 : 0);
#else
        Serial.println("NAPT 0");
#endif
    } else if (strcmp(line, "STATS") == 0) {
        tinywg::Stats s = wg.stats();
        tinywg::PeerStatus p;
        wg.peerStatus(peer, &p);
        out("STATS init=%u done=%u resp=%u badmac=%u hsfail=%u unknown=%u replay=%u decrypt=%u "
                      "notallowed=%u up=%d rx=%lu tx=%lu heap=%u\n",
                      (unsigned)s.handshakesInitiated, (unsigned)s.handshakesCompleted,
                      (unsigned)s.handshakesResponded, (unsigned)s.rxBadMac1, (unsigned)s.rxHandshakeFail,
                      (unsigned)s.rxUnknownPeer, (unsigned)s.rxReplay, (unsigned)s.rxDecryptFail,
                      (unsigned)s.rxNotAllowed, p.sessionUp ? 1 : 0, (unsigned long)p.rxBytes,
                      (unsigned long)p.txBytes, freeHeap());
    }
}

#if defined(ARDUINO_ARCH_ESP32)
static bool networkUp()
{
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("NET_ERROR missing_ssid (set TEST_WIFI_SSID in tests/.env)");
        return false;
    }
    WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
        out("WIFI_DISCONNECT reason=%d\n", (int)info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (uint32_t start = millis(); WiFi.status() != WL_CONNECTED && millis() - start < 120000;) delay(250);
    if (WiFi.status() == WL_CONNECTED) return true;
    // Say whether the AP is visible at all (ESP32 sees 2.4 GHz only).
    WiFi.disconnect();
    delay(200);
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i) == WIFI_SSID) {
            out("WIFI_SCAN found rssi=%d channel=%d auth=%d\n", (int)WiFi.RSSI(i), (int)WiFi.channel(i), (int)WiFi.encryptionType(i));
        }
    }
    out("NET_ERROR wifi_connect_failed status=%d visible_networks=%d\n", (int)WiFi.status(), n);
    return false;
}
#else
static bool networkUp()
{
    if (!ETH.begin()) {
        Serial.println("NET_ERROR eth_begin");
        return false;
    }
    for (uint32_t start = millis(); !ETH.hasIP() && millis() - start < 30000;) delay(100);
    if (ETH.hasIP()) return true;
    out("NET_ERROR eth_no_ip link=%d\n", ETH.linkUp() ? 1 : 0);
    return false;
}
#endif

void setup()
{
    Serial.begin(115200);
    delay(5000);
    if (!networkUp()) return;
    Serial.print("NET_OK ");
    Serial.println(localIP());
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
