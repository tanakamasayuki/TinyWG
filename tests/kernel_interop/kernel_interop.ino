// DUT for the Linux kernel WireGuard interop tests (host profile only: the
// kernel peer lives in the same network namespace, see test_kernel_interop.py).
//
// TinyWGUdp over the host core's WiFiUDP, tunnel address 10.9.0.2. Inside the
// tunnel it answers ICMP echo and UDP echo (port 7), and on request sends a
// UDP "hello" to 10.9.0.1:9000.
//
// Serial commands (one line each):
//   CFG <private> <peer public> <listen port> <peer port, 0 = none> [<psk>]
//   HELLO        send the hello datagram
//   GUARD <n>    a CookieGuard under load above n handshakes a second; -1 removes it
//   STATS        print the drop counters

#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>
#include <string.h>

static WiFiUDP udp;
static TinyWGUdp wg;
static tinywg::CookieGuard guard;
static bool configured = false;

static uint16_t checksum(const uint8_t *p, size_t n, uint32_t sum = 0)
{
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void finishIpHeader(uint8_t *p, size_t headerLen)
{
    p[10] = p[11] = 0;
    uint16_t c = checksum(p, headerLen);
    p[10] = (uint8_t)(c >> 8);
    p[11] = (uint8_t)c;
}

// Turns an ICMP echo request or a UDP datagram to port 7 into its reply.
static void onPacket(int peer, uint8_t *p, size_t len, void *)
{
    size_t hl = (size_t)(p[0] & 0x0F) * 4;
    uint8_t proto = p[9];
    Serial.printf("RX peer=%d proto=%u len=%u\n", peer, proto, (unsigned)len);
    uint8_t tmp[4];
    if (proto == 1 && p[hl] == 8) {
        p[hl] = 0;
        p[hl + 2] = p[hl + 3] = 0;
        uint16_t c = checksum(p + hl, len - hl);
        p[hl + 2] = (uint8_t)(c >> 8);
        p[hl + 3] = (uint8_t)c;
    } else if (proto == 17 && ((p[hl + 2] << 8) | p[hl + 3]) == 7) {
        memcpy(tmp, p + hl, 2);
        memcpy(p + hl, p + hl + 2, 2);
        memcpy(p + hl + 2, tmp, 2);
        p[hl + 6] = p[hl + 7] = 0;   // UDP checksum is optional over IPv4
    } else {
        return;
    }
    memcpy(tmp, p + 12, 4);
    memcpy(p + 12, p + 16, 4);
    memcpy(p + 16, tmp, 4);
    finishIpHeader(p, hl);
    tinywg::SendResult r = wg.send(p, len);
    Serial.printf("TX result=%d\n", (int)r);
}

static void sendHello()
{
    static const char msg[] = "hello-from-tinywg";
    uint8_t p[28 + sizeof(msg) - 1];
    size_t n = sizeof(p);
    memset(p, 0, n);
    p[0] = 0x45;
    p[2] = (uint8_t)(n >> 8);
    p[3] = (uint8_t)n;
    p[8] = 64;
    p[9] = 17;
    const uint8_t src[4] = {10, 9, 0, 2}, dst[4] = {10, 9, 0, 1};
    memcpy(p + 12, src, 4);
    memcpy(p + 16, dst, 4);
    finishIpHeader(p, 20);
    p[20] = 5555 >> 8;
    p[21] = 5555 & 0xFF;
    p[22] = 9000 >> 8;
    p[23] = 9000 & 0xFF;
    p[24] = (uint8_t)((n - 20) >> 8);
    p[25] = (uint8_t)(n - 20);
    memcpy(p + 28, msg, sizeof(msg) - 1);
    Serial.printf("HELLO result=%d\n", (int)wg.send(p, n));
}

static void configure(char *line)
{
    char *argv[6] = {nullptr};
    int argc = 0;
    for (char *tok = strtok(line, " "); tok && argc < 6; tok = strtok(nullptr, " ")) argv[argc++] = tok;
    if (argc < 5) {
        Serial.println("DUT-ERROR usage");
        return;
    }
    if (!wg.begin(udp, (uint16_t)atoi(argv[3]), argv[1])) {
        Serial.println("DUT-ERROR begin");
        return;
    }
    int peer = wg.addPeer(argv[2], argc > 5 ? argv[5] : nullptr);
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 0), 24);
    int peerPort = atoi(argv[4]);
    if (peerPort > 0) wg.setEndpoint(peer, IPAddress(127, 0, 0, 1), (uint16_t)peerPort);
    wg.onPacket(onPacket);
    configured = peer >= 0;
    char pub[45];
    wg.publicKey(pub);
    Serial.printf("DUT-READY peer=%d pub=%s\n", peer, pub);
}

static void command(char *line)
{
    if (strncmp(line, "CFG ", 4) == 0) {
        configure(line);
    } else if (strcmp(line, "HELLO") == 0) {
        sendHello();
    } else if (strncmp(line, "GUARD ", 6) == 0) {
        int n = atoi(line + 6);
        if (n >= 0) guard = tinywg::CookieGuard((uint8_t)n);
        wg.setCookieGuard(n >= 0 ? &guard : nullptr);
        Serial.printf("GUARD %d\n", n);
    } else if (strcmp(line, "STATS") == 0) {
        tinywg::Stats s = wg.stats();
        Serial.printf("STATS init=%u done=%u resp=%u badmac=%u hsfail=%u stale=%u replay=%u decrypt=%u notallowed=%u "
                      "cookies=%u underload=%u\n",
                      (unsigned)s.handshakesInitiated, (unsigned)s.handshakesCompleted,
                      (unsigned)s.handshakesResponded, (unsigned)s.rxBadMac1, (unsigned)s.rxHandshakeFail,
                      (unsigned)s.rxStaleTimestamp, (unsigned)s.rxReplay, (unsigned)s.rxDecryptFail,
                      (unsigned)s.rxNotAllowed, (unsigned)s.cookiesSent, (unsigned)s.rxUnderLoad);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    WiFi.mode(WIFI_STA);
    WiFi.begin("host", "host");
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
    if (configured) wg.update();
    delay(1);
}
