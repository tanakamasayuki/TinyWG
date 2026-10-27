# Ch32EthernetClient

[日本語](README.ja.md) | [Beginner's guide](../../docs/guide-beginner.md)

Connects to a WireGuard server and sends `hello from TinyWG` over UDP to `10.9.0.1:9000` through the tunnel every 5 seconds. It uses the ordinary UDP class: the destination is a tunnel address, so lwIP sends it into `wg0`.

## What you need

- A CH32V with built-in Ethernet (CH32V307 and others; ArduinoCore-CH32RV 0.4 or later, Network library)
- A WireGuard server (Linux `wg-quick`, the official apps, ...)

## Settings

| Constant | Meaning |
|---|---|
| Network | Ethernet (`ETH.begin()`, DHCP); nothing to set |
| `kPrivateKey` | This device's private key (`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | This device's tunnel address (`10.9.0.3/24` by default) |
| `kServerPublicKey` | The server's public key |
| `kServerEndpoint` / `kServerPort` | The server's outer address and port |
| `kServerTunnelIp` | The server's tunnel address (`10.9.0.1` by default) |

The clock: `configTime()` sets it over NTP (waits up to 10 s). A client puts the time into its handshakes.

## Server side (Linux example)

```ini
[Interface]
PrivateKey = <server private key>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <this device's public key (printed on the serial port at start-up)>
AllowedIPs = 10.9.0.3/32
```

## What you see

On the serial port (115200 baud):

```
TinyWG Ch32EthernetClient
Local IP: 192.168.1.50
Public key: <this device's public key>
peer up: yes
```

Run `nc -lu 9000` on the server and a message arrives every 5 seconds. `ping 10.9.0.3` is answered too.

## Notes

- Random numbers come from the TRNG. On series without one (CH32V208 and others) `wg.begin()` fails; supply a source with `wg.setRandom()` ([advanced guide](../../docs/guide-advanced.md) §7)
- lwIP runs after `loop()` and inside `delay()`; do not block `loop()` for long
- Verified on a CH32V307: traffic through the tunnel and the long run
- If it does not work, see the [beginner's guide](../../docs/guide-beginner.md) §11
