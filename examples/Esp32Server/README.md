# Esp32Server

[日本語](README.ja.md) | [Beginner's guide](../../docs/guide-beginner.md)

Makes an ESP32 a WireGuard server for two peers. It runs a TCP echo server (port 7) reachable only through the tunnel, and prints each peer's state on the serial port every 10 seconds.

## What you need

- An ESP32-family board such as ESP32 / ESP32-S3 (arduino-esp32 3.3 or later)
- WireGuard peers that connect to it (Linux `wg-quick`, the official apps on PCs and phones, ...)

## Settings

| Constant | Meaning |
|---|---|
| `kWifiSsid` / `kWifiPass` | Wi-Fi |
| `kPrivateKey` | This server's private key (`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | The server's tunnel address (`10.9.0.1/24` by default) |
| `kListenPort` | UDP port to listen on (51820 by default) |
| `kPeers` | Each peer's public key and tunnel address (`10.9.0.2` and `10.9.0.3` by default) |
| `kTimestampReset` | Accept a client with no clock this many seconds after it last talked (30 by default; 0 refuses, as standard) |
| `kTimeServer` | Hand out this server's time over NTP on UDP port 123 of its LAN address (true by default) |

No peer IP address (Endpoint) is needed: the server learns it from the first handshake and follows the peer if it moves.

The server's clock does not matter for connecting. At start-up it tries to set it from `pool.ntp.org`; without the Internet it counts from 1970. Clients can use the server's LAN address as their NTP server; the time is the real one only when the server could set its clock over the Internet ([beginner's guide §8](../../docs/guide-beginner.md#8-about-the-clock-important)).

## Peer side

```ini
[Interface]
PrivateKey = <peer private key>
Address = 10.9.0.2/24

[Peer]
PublicKey = <the server's public key (printed on the serial port at start-up)>
Endpoint = <the server's LAN address>:51820
AllowedIPs = 10.9.0.1/32
PersistentKeepalive = 25
```

For the second peer, use `Address = 10.9.0.3/24`.

## What you see

```
TinyWG Esp32Server
Endpoint: 192.168.1.50:51820
Public key: <the server's public key>
peer 0: up rx=1234 tx=1180
peer 1: down rx=0 tx=0
```

From a peer, `nc 10.9.0.1 7` echoes back what you type. `ping 10.9.0.1` is answered too.

## Going further

- Add `wg.enableNapt()` and peers can go on through the server to the home LAN or the Internet. Add the LAN to the peer's AllowedIPs ([advanced guide](../../docs/guide-advanced.md) §4)
- For more than 4 peers, raise `TINYWG_MAX_PEERS` at build time
- Before putting it where the Internet can reach it directly, read the security notes in the advanced guide §13
