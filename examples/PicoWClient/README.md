# PicoWClient

[日本語](README.ja.md) | [Beginner's guide](../../docs/guide-beginner.md)

Connects to a WireGuard server and sends `hello from TinyWG` over UDP to `10.9.0.1:9000` through the tunnel every 5 seconds. It uses the ordinary UDP class: the destination is a tunnel address, so lwIP sends it into `wg0`.

## What you need

- Raspberry Pi Pico W / Pico 2 W (arduino-pico 6.3 or later)
- A WireGuard server (Linux `wg-quick`, the official apps, ...)

## Settings

| Constant | Meaning |
|---|---|
| Network | Wi-Fi (`kWifiSsid` / `kWifiPass`) |
| `kPrivateKey` | This device's private key (`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | This device's tunnel address (`10.9.0.4/24` by default) |
| `kServerPublicKey` | The server's public key |
| `kServerEndpoint` / `kServerPort` | The server's outer address and port |
| `kServerTunnelIp` | The server's tunnel address (`10.9.0.1` by default) |

The clock: `NTP.begin()` / `NTP.waitSet()` set it. A client puts the time into its handshakes.

## Server side (Linux example)

```ini
[Interface]
PrivateKey = <server private key>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <this device's public key (printed on the serial port at start-up)>
AllowedIPs = 10.9.0.4/32
```

## What you see

On the serial port (115200 baud):

```
TinyWG PicoWClient
Local IP: 192.168.1.50
Public key: <this device's public key>
peer up: yes
```

Run `nc -lu 9000` on the server and a message arrives every 5 seconds. `ping 10.9.0.4` is answered too.

## Notes

- Build-tested only for now; not yet run on hardware
- arduino-pico's FreeRTOS mode is not supported (it is a compile error)
- If it does not work, see the [beginner's guide](../../docs/guide-beginner.md) §11
