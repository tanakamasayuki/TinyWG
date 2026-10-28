// TinyWGTimeServer on the host core: port 12300 serves a fixed clock
// (1000000000.5 Unix), port 12301 the default clock. test_time_server.py
// sends SNTP requests to both.

#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>

static WiFiUDP fixedUdp, defaultUdp;
static TinyWGTimeServer fixedServer, defaultServer;

static void fixedClock(uint64_t *seconds, uint32_t *nanoseconds)
{
    *seconds = 1000000000;
    *nanoseconds = 500000000;
}

void setup()
{
    Serial.begin(115200);
    fixedServer.setClock(fixedClock);
    bool ok = fixedServer.begin(fixedUdp, 12300) && defaultServer.begin(defaultUdp, 12301);
    Serial.println(ok ? "TIME-READY" : "TIME-FAIL");
}

void loop()
{
    fixedServer.update();
    defaultServer.update();
    delay(1);
}
