# TinyWG

[日本語](README.ja.md)

A portable WireGuard implementation for Arduino. It works as **both initiator (client) and responder (server)** and handles several peers.

- The protocol engine (`tinywg::Device`) depends on neither lwIP nor the Arduino API.
- `TinyWG` is an lwIP network interface (wg0): existing lwIP code — `WiFiClient` / `NetworkClient`, `WiFiServer`, an MQTT broker — runs through the tunnel unchanged.
- `TinyWGUdp` runs over any Arduino `UDP`; the sketch handles the inner IPv4 packets itself.
- Interoperability with the Linux kernel WireGuard is covered by automated tests.

It is meant for microcontrollers where TLS is too heavy or not available: a WireGuard tunnel needs less flash and RAM than TLS.

## Status

| Core | `TinyWG` (lwIP) | `TinyWGUdp` | Random source | Verified |
|---|---|---|---|---|
| ESP32 / ESP32-S3 (arduino-esp32 3.3) | yes (NAPT available) | yes | `esp_fill_random` | on board (two-board TCP, benchmarks) |
| RP2040 / RP2350 (arduino-pico 6.3, Pico W / Pico 2 W) | yes (not in FreeRTOS mode) | yes | `get_rand_64` | build only |
| CH32V (ArduinoCore-CH32RV 0.4, Network library) | yes | yes | TRNG (V307 and others) | build only |
| lang-ship:host (tests on a PC) | — | yes | `/dev/urandom` | interop-tested against the Linux kernel WireGuard |

On an ESP32, one X25519 takes 17.6 ms and answering a handshake 72.7 ms (ESP32-S3: 12.5 ms / 52.0 ms); see SPEC.ja.md §5.2.

### Footprint (added to an otherwise identical sketch)

| | Flash | RAM (static) |
|---|---|---|
| ESP32 | +25.6 KB | +8.2 KB |
| Pico W | +22.9 KB | +7.9 KB |
| CH32V307 | +22.2 KB | +7.9 KB |

Default configuration (4 peers, MTU 1420). No heap allocation.

## Usage (client)

```cpp
#include <WiFi.h>
#include <TinyWG.h>

TinyWG wg;

void setup()
{
    // ... join Wi-Fi and set the clock with NTP (see "Clock" below)
    wg.begin(IPAddress(10, 9, 0, 2), IPAddress(255, 255, 255, 0), "<this device private key>", 0);
    int server = wg.addPeer("<server public key>");
    wg.addAllowedIp(server, IPAddress(10, 9, 0, 0), 24);
    wg.setEndpoint(server, IPAddress(203, 0, 113, 1), 51820);
    wg.setPersistentKeepalive(server, 25);
}
```

After `begin()`, lwIP routes traffic for 10.9.0.0/24 into wg0. Nothing needs to be called from `loop()`.

## Usage (server)

```cpp
wg.begin(IPAddress(10, 9, 0, 1), IPAddress(255, 255, 255, 0), "<server private key>", 51820);
int a = wg.addPeer("<peer A public key>");
wg.addAllowedIp(a, IPAddress(10, 9, 0, 2), 32);
int b = wg.addPeer("<peer B public key>");
wg.addAllowedIp(b, IPAddress(10, 9, 0, 3), 32);
```

Peers that connect to us need no endpoint; it follows the source of the last authenticated packet (roaming). On ESP32, `wg.enableNapt()` relays what peers send to the LAN or the Internet through NAPT.

Keys come from `wg genkey` / `wg pubkey`.

## Clock

The initiator's handshake carries a TAI64N timestamp, and the responder accepts only newer ones. A board without an RTC whose clock goes back after a reboot is rejected by its peer (Linux included). **When acting as a client, set the clock with NTP before `begin()`.** A responder-only device needs no clock. `setWallClock()` replaces the time source.

## Build-time configuration

Override with `-D` (`src/tinywg/wg_config.h`).

| Macro | Default | |
|---|---|---|
| `TINYWG_MAX_PEERS` | 4 | peers |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | AllowedIPs entries per peer |
| `TINYWG_MTU` | 1420 | inner MTU |
| `TINYWG_REPLAY_WINDOW` | 128 | replay window in bits |
| `TINYWG_STAGED_PACKET` | 1 | hold one packet while a handshake is in progress |

## Limitations

- IPv4 only, inside and outside the tunnel.
- Cookie replies are received but never sent (no under-load mode).
- arduino-pico in FreeRTOS mode is not supported yet.
- If wg0 is made the default route, outer datagrams that would themselves enter the tunnel are dropped (loop prevention).

## License

BSD 3-Clause. The bundled [Monocypher](src/third_party/monocypher/) (X25519, ChaCha20-Poly1305) is dual-licensed BSD-2-Clause / CC0. BLAKE2s is written from RFC 7693 and the protocol layer from the WireGuard whitepaper; no GPL code is used.

"WireGuard" is a registered trademark of Jason A. Donenfeld. This library is not affiliated with the WireGuard project; the protocol name is used descriptively.
