// A minimal SNTP server (RFC 4330) over any Arduino UDP. It answers with this
// device's clock, so clients that point NTP at the WireGuard server's address
// share its time. That is the real time only if this device's clock is set
// (NTP, RTC); otherwise it is the time since boot, the same on every client.
// Costs nothing when not used.
#pragma once

#include <Udp.h>

#include "tinywg/wg_port.h"

class TinyWGTimeServer {
public:
    // Opens udp on port. Use a UDP object of its own.
    bool begin(UDP &udp, uint16_t port = 123);
    void end();
    // Answers the pending requests. Call from loop().
    void update();
    // The clock served. The default is the one TinyWG uses (gettimeofday()).
    void setClock(tinywg::WallClockFn fn) { clock_ = fn; }

private:
    void ntpTime(uint8_t out[8]);

    UDP *udp_ = nullptr;
    tinywg::WallClockFn clock_ = tinywg::port::defaultWallClock;
};
