// Board-to-board test, client side: joins the server board's access point,
// runs TinyWG as the initiator (wg0 = 10.9.0.2/24) and pushes data through
// the tunnel to the server's TCP echo on 10.9.0.1:7.
//
// There is no NTP here: the handshake timestamp comes from the clock since
// boot, which a freshly booted server accepts.
//
// Serial: CFG <private> <server public> <ap ssid> <ap password> <server ip>
//         RUN <bytes>     TCP echo of <bytes> bytes, reports time and result

#include <WiFi.h>
#include <TinyWG.h>
#include <string.h>

static TinyWG wg;
static int peer = -1;

static void configure(char *line)
{
    char *argv[6] = {nullptr};
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 6; tok = strtok(nullptr, " ")) argv[argc++] = tok;
    if (argc < 6) {
        Serial.println("CLIENT-ERROR usage");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(argv[3], argv[4]);
    for (uint32_t start = millis(); WiFi.status() != WL_CONNECTED && millis() - start < 30000;) delay(100);
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("CLIENT-ERROR wifi %d\n", (int)WiFi.status());
        return;
    }
    IPAddress server;
    server.fromString(argv[5]);
    if (!wg.begin(IPAddress(10, 9, 0, 2), IPAddress(255, 255, 255, 0), argv[1], 0)) {
        Serial.println("CLIENT-ERROR begin");
        return;
    }
    peer = wg.addPeer(argv[2]);
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 1), 32);
    wg.setEndpoint(peer, server, 51820);
    uint32_t t = millis();
    wg.connect(peer);
    while (!wg.peerUp(peer) && millis() - t < 10000) delay(5);
    if (!wg.peerUp(peer)) {
        Serial.println("CLIENT-ERROR handshake");
        return;
    }
    Serial.printf("CLIENT-READY handshake_ms=%lu\n", (unsigned long)(millis() - t));
}

static void run(size_t total)
{
    WiFiClient c;
    uint32_t t = millis();
    if (!c.connect(IPAddress(10, 9, 0, 1), 7, 5000)) {
        Serial.println("TCP-ERROR connect");
        return;
    }
    uint32_t connectMs = millis() - t;
    static uint8_t out[1024], in[1024];
    size_t sent = 0, received = 0;
    bool same = true;
    t = millis();
    while (received < total && millis() - t < 30000) {
        if (sent < total && sent - received < 4096) {
            size_t n = total - sent < sizeof(out) ? total - sent : sizeof(out);
            for (size_t i = 0; i < n; i++) out[i] = (uint8_t)((sent + i) * 13);
            sent += c.write(out, n);
        }
        int n = c.read(in, sizeof(in));
        for (int i = 0; i < n; i++) same &= in[i] == (uint8_t)((received + i) * 13);
        if (n > 0) received += n;
        else delay(1);
    }
    uint32_t ms = millis() - t;
    c.stop();
    Serial.printf("TCP-ECHO bytes=%u received=%u match=%d connect_ms=%lu ms=%lu kib_s=%.1f\n", (unsigned)total,
                  (unsigned)received, same ? 1 : 0, (unsigned long)connectMs, (unsigned long)ms,
                  ms ? total * 2 / 1.024 / ms : 0.0);
}

void setup()
{
    Serial.begin(115200);
    delay(5000);
    Serial.println("CLIENT-WAIT-CONFIG");
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
            if (strncmp(line, "CFG ", 4) == 0) configure(line);
            else if (strncmp(line, "RUN ", 4) == 0) run((size_t)atol(line + 4));
            len = 0;
        } else if (len + 1 < sizeof(line)) {
            line[len++] = (char)c;
        }
    }
    delay(1);
}
