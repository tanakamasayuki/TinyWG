// Board-to-board test, server side: this board opens its own Wi-Fi access
// point (no external AP needed) and runs TinyWG as the responder, wg0 =
// 10.9.0.1/24, with a TCP echo server on port 7 reachable only through the
// tunnel. The client is peer_client/ on a second board.
//
// Serial: CFG <private> <client public> <ap ssid> <ap password>
//         STATS

#include <WiFi.h>
#include <TinyWG.h>
#include <string.h>

static TinyWG wg;
static WiFiServer echo(7);
static int peer = -1;
static bool ready = false;

static void configure(char *line)
{
    char *argv[5] = {nullptr};
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 5; tok = strtok(nullptr, " ")) argv[argc++] = tok;
    if (argc < 5) {
        Serial.println("SERVER-ERROR usage");
        return;
    }
    WiFi.mode(WIFI_AP);
    if (!WiFi.softAP(argv[3], argv[4])) {
        Serial.println("SERVER-ERROR softAP");
        return;
    }
    if (!wg.begin(IPAddress(10, 9, 0, 1), IPAddress(255, 255, 255, 0), argv[1], 51820)) {
        Serial.println("SERVER-ERROR begin");
        return;
    }
    peer = wg.addPeer(argv[2]);
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 2), 32);
    echo.begin();
    ready = true;
    Serial.print("SERVER-READY ap_ip=");
    Serial.println(WiFi.softAPIP());
}

static void command(char *line)
{
    if (strncmp(line, "CFG ", 4) == 0) {
        configure(line);
    } else if (strcmp(line, "STATS") == 0) {
        tinywg::Stats s = wg.stats();
        tinywg::PeerStatus p;
        wg.peerStatus(peer, &p);
        Serial.printf("STATS resp=%u done=%u replay=%u decrypt=%u hsfail=%u up=%d rx=%llu tx=%llu\n",
                      (unsigned)s.handshakesResponded, (unsigned)s.handshakesCompleted, (unsigned)s.rxReplay,
                      (unsigned)s.rxDecryptFail, (unsigned)s.rxHandshakeFail, p.sessionUp ? 1 : 0,
                      (unsigned long long)p.rxBytes, (unsigned long long)p.txBytes);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("SERVER-WAIT-CONFIG");
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
    if (!ready) return;
    WiFiClient client = echo.accept();
    if (client) {
        Serial.print("SERVER-CLIENT ");
        Serial.println(client.remoteIP());
        static uint8_t buf[1024];
        while (client.connected()) {
            int n = client.read(buf, sizeof(buf));
            if (n > 0) client.write(buf, n);
            else delay(1);
        }
        client.stop();
    }
    delay(1);
}
