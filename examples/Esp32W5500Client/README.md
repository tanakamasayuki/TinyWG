# Esp32W5500Client

[日本語](README.ja.md) | [Advanced guide §2](../../docs/guide-advanced.md#replacing-the-outer-udp)

An ESP32 with a W5500 on Arduino's `Ethernet` library connects to a WireGuard server and, every 5 s, sends the UDP message `hello from TinyWG over W5500` to `10.9.0.1:9000` through the tunnel.

With the `Ethernet` library, TCP/IP runs inside the W5500, outside the ESP32's lwIP. So `wg0` stays in lwIP, and `setOuterUdp()` sends only the encrypted outer UDP through the W5500 (`EthernetUDP`). Inside the tunnel it is lwIP, so `NetworkUDP` and `NetworkClient` work as they are.

When the W5500 runs under the ESP32's `ETH` (on lwIP), this example is not needed: plain `TinyWG` works, as in `Esp32Client`.

## What you need

- An ESP32 (arduino-esp32 3.3 or later) and a W5500 module
- Arduino's `Ethernet` library 2.0.2 (the `sketch.yaml` profile installs it; in the Arduino IDE use the Library Manager)
- A WireGuard server (Linux `wg-quick`, an official app, ...)

## Wiring

| W5500 | ESP32 |
|---|---|
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| CS | GPIO 5 (`kCsPin`) |
| 3.3V / GND | 3.3V / GND |

## Settings

| Constant | Meaning |
|---|---|
| `kCsPin` | The W5500's chip select pin |
| `kMac` | The W5500's MAC address (unique on the LAN) |
| `kPrivateKey` | This device's private key (`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | This device's tunnel address (`10.9.0.2/24` by default) |
| `kServerPublicKey` | The server's public key |
| `kServerEndpoint` / `kServerPort` | The server's outer address and port |
| `kServerTunnelIp` | The server's tunnel address (`10.9.0.1` by default) |
| `kNtpServer` | The NTP server for the clock, queried through the W5500 |

The address comes from DHCP. The clock is set once at start-up by SNTP through the W5500; lwIP's `configTime()` cannot be used, as lwIP has no way out. On a network with no Internet, name an NTP server inside it ([beginner's guide §8](../../docs/guide-beginner.md#8-about-the-clock-important)).

The server side is set up as for [`Esp32Client`](../Esp32Client/README.md#server-side-linux-example).

## How the outer side is driven

This example calls `wg.setOuterUdp(&outer, true)`, so `wg.update()` in `loop()` drives the W5500. `loop()` also calls `Ethernet.maintain()` (DHCP renewal), and this keeps every use of the W5500 in `loop()`. In this mode, the tunnel stops whenever `loop()` blocks for long.

For code that waits for the other side, such as MQTT or HTTP, use `wg.setOuterUdp(&outer)` (no second argument). The library then drives the W5500 from a task of its own, so the tunnel keeps going while `loop()` waits. In exchange the sketch must not use the W5500: stop calling `Ethernet.maintain()` and use a static address (`Ethernet.begin(kMac, ip, dns, gateway, subnet)`) so no DHCP renewal is needed.

## What you see

On the serial port (115200 bps):

```
TinyWG Esp32W5500Client
Local IP: 192.168.1.60
clock set
Public key: <this device's public key>
peer up: yes
```

Run `nc -lu 9000` on the server and a message arrives every 5 s. `ping 10.9.0.2` is answered too.

## Notes

- Not yet tried on a real W5500 (built, and tested with an ESP32 `WiFiUDP` as the outer side)
- `W5500 not found` means checking the wiring and `kCsPin`
