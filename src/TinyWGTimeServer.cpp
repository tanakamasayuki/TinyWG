#include "TinyWGTimeServer.h"

#include <string.h>

namespace {

constexpr size_t kPacketLen = 48;
constexpr uint8_t kModeClient = 3;
constexpr uint8_t kModeServer = 4;
constexpr uint8_t kStratum = 10;            // a local clock, as chrony's "local stratum 10"
constexpr uint64_t kUnixToNtp = 2208988800u;  // seconds from 1900 to 1970

}  // namespace

bool TinyWGTimeServer::begin(UDP &udp, uint16_t port)
{
    end();
    if (!udp.begin(port)) return false;
    udp_ = &udp;
    return true;
}

void TinyWGTimeServer::end()
{
    if (udp_) udp_->stop();
    udp_ = nullptr;
}

void TinyWGTimeServer::ntpTime(uint8_t out[8])
{
    uint64_t seconds;
    uint32_t nanoseconds;
    clock_(&seconds, &nanoseconds);
    uint32_t s = (uint32_t)(seconds + kUnixToNtp);   // the NTP era wraps in 2036, as clients expect
    uint32_t f = (uint32_t)(((uint64_t)nanoseconds << 32) / 1000000000u);
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(s >> (24 - 8 * i));
        out[4 + i] = (uint8_t)(f >> (24 - 8 * i));
    }
}

void TinyWGTimeServer::update()
{
    if (!udp_) return;
    while (udp_->parsePacket() > 0) {
        uint8_t req[kPacketLen];
        uint8_t received[8];
        ntpTime(received);
        if (udp_->read(req, kPacketLen) != (int)kPacketLen || (req[0] & 7) != kModeClient) continue;

        uint8_t resp[kPacketLen];
        memset(resp, 0, sizeof(resp));
        resp[0] = (uint8_t)((req[0] & 0x38) | kModeServer);   // no leap warning, the client's version
        resp[1] = kStratum;
        resp[2] = req[2];                                      // poll interval
        resp[3] = (uint8_t)-10;                                // precision about 1 ms
        memcpy(resp + 12, "LOCL", 4);                          // reference ID
        memcpy(resp + 16, received, 8);                        // reference time
        memcpy(resp + 24, req + 40, 8);                        // origin: the client's transmit time
        memcpy(resp + 32, received, 8);                        // receive time
        ntpTime(resp + 40);                                    // transmit time
        udp_->beginPacket(udp_->remoteIP(), udp_->remotePort());
        udp_->write(resp, sizeof(resp));
        udp_->endPacket();
    }
}
