# Beginner's guide

[日本語](guide-beginner.ja.md) | [Advanced guide](guide-advanced.md) | [API reference](api.md) | [README](../README.md)

This guide connects an ESP32 to a Linux PC with WireGuard and checks that traffic flows through the tunnel. You only need to have uploaded an Arduino sketch before; no WireGuard knowledge is assumed.

It takes about 30 minutes.

## 1. WireGuard and TinyWG

WireGuard builds an encrypted "tunnel" between two devices. Inside the tunnel each device has its own IP address (for example `10.9.0.1` and `10.9.0.2`), and whatever is sent to that address is encrypted on the way and reaches the other side.

```
  PC (10.9.0.1)                                   ESP32 (10.9.0.2)
     │                                                  │
     └──── encrypted UDP (your LAN or the Internet) ────┘
```

TinyWG runs WireGuard on a microcontroller. The other side can be any ordinary WireGuard: Linux, or the official apps for Windows, macOS and phones.

On the ESP32, TinyWG works as a network interface, `wg0`. Your usual `WiFiClient` and `WiFiUDP` code reaches addresses inside the tunnel (`10.9.0.1` and so on) as it is.

## 2. What you need

- **An ESP32 board**: ESP32 or ESP32-S3; both are verified on hardware. A CH32V307 (Ethernet) or a Pico W works too: see [§9](#9-boards-other-than-the-esp32)
- **Arduino IDE 2** or `arduino-cli`
  - with the ESP32 board package (arduino-esp32) 3.3 or later
- **A Linux PC** (Ubuntu or similar) with `wireguard-tools` (`sudo apt install wireguard-tools`)
  - Windows and macOS work the same way with the official app; the settings are those of §5
- The ESP32 and the PC on the same Wi-Fi (LAN)

## 3. Install the library

TinyWG is not in the Library Manager yet. Install it from GitHub.

**Arduino IDE**: download the ZIP from the GitHub page and use Sketch → Include Library → Add .ZIP Library.

**git**: clone it into your Arduino libraries folder.

```sh
cd ~/Arduino/libraries
git clone https://github.com/tanakamasayuki/TinyWG.git
```

The examples then appear under File → Examples → TinyWG.

## 4. Make the keys

In WireGuard every device has one pair of keys:

- **Private key**: known only to that device. Never give it away
- **Public key**: computed from the private key. You give it to the other side, which writes it in its settings

On the PC, make one pair for the PC and one for the ESP32.

```sh
wg genkey | tee pc.key | wg pubkey > pc.pub
wg genkey | tee esp32.key | wg pubkey > esp32.pub
cat pc.key pc.pub esp32.key esp32.pub
```

You get four 44-character strings, one per line (like `yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=`). They go here:

| File | Where it goes |
|---|---|
| `pc.key` | `PrivateKey` in the PC's settings |
| `pc.pub` | `kServerPublicKey` in the ESP32 sketch |
| `esp32.key` | `kPrivateKey` in the ESP32 sketch |
| `esp32.pub` | `PublicKey` under `[Peer]` in the PC's settings |

Each side uses its own private key and the other side's public key. Mixing them up is the most common reason a tunnel does not come up.

## 5. Set up the PC (the WireGuard server)

Create `/etc/wireguard/wg0.conf` with the keys from §4 in place of `<...>`.

```ini
[Interface]
PrivateKey = <contents of pc.key>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
# ESP32
PublicKey = <contents of esp32.pub>
AllowedIPs = 10.9.0.2/32
```

- `Address`: the PC's address inside the tunnel
- `ListenPort`: the UDP port the PC listens on for WireGuard
- `AllowedIPs`: the tunnel addresses this peer (the ESP32) may use

Bring the tunnel up and note the PC's LAN address.

```sh
sudo wg-quick up wg0
ip -4 addr show     # e.g. 192.168.1.10, used in the ESP32 sketch
```

If you use a firewall (ufw), allow WireGuard's UDP port and traffic that arrives through the tunnel.

```sh
sudo ufw allow 51820/udp
sudo ufw allow in on wg0
```

## 6. The ESP32 sketch

In the Arduino IDE open File → Examples → TinyWG → Esp32Client and edit the settings at the top.

```cpp
static const char *kWifiSsid = "your-ssid";          // Wi-Fi name
static const char *kWifiPass = "your-passphrase";    // Wi-Fi password

static const char *kPrivateKey = "<contents of esp32.key>";
static const IPAddress kTunnelIp(10, 9, 0, 2);       // the PC's AllowedIPs
static const IPAddress kTunnelMask(255, 255, 255, 0);

static const char *kServerPublicKey = "<contents of pc.pub>";
static const IPAddress kServerEndpoint(192, 168, 1, 10);   // the PC's LAN address
static constexpr uint16_t kServerPort = 51820;             // the PC's ListenPort
static const IPAddress kServerTunnelIp(10, 9, 0, 1);       // the PC's Address
```

The sketch does this, in order:

1. Joins Wi-Fi
2. Sets the clock over NTP (see §8 for why)
3. Brings up the tunnel interface `wg0` with `wg.begin()`
4. Registers the PC with `wg.addPeer()`, then `addAllowedIp()`, `setEndpoint()` and `setPersistentKeepalive()`
5. In `loop()`, sends `hello from TinyWG` to `10.9.0.1:9000` with `WiFiUDP` every 5 seconds

Step 5 uses no tunnel-specific call. The destination is a tunnel address, so the ESP32's network stack (lwIP) sends it into `wg0` by itself.

Upload it and open the Serial Monitor at 115200 baud.

## 7. Check the connection

`peer up: yes` on the Serial Monitor means the handshake (the key exchange) is done.

Check from the PC too:

```sh
sudo wg show                 # "latest handshake: ... seconds ago" means connected
ping 10.9.0.2                # the ESP32 answers from inside the tunnel
nc -lu 9000                  # "hello from TinyWG" arrives every 5 s
```

That's it: everything between the ESP32 and the PC is now encrypted.

## 8. About the clock (important)

The side that starts the connection (the client) puts the current time into every handshake. The other side refuses a handshake whose time is not newer than the last one, as it might be a replay.

Whether the time is correct is not checked: it only has to be later than the last one.

Most microcontrollers restart their clock at 1970 on every power-up. Left like that, the other side refuses every boot after the first. Either of these fixes it:

- **Set the client's clock**: the example sets it over NTP before `wg.begin()`. The easiest way when the device can reach the Internet
- **When the server is TinyWG, call `wg.setTimestampReset(30)` on the server**: a client it has not heard from for 30 s is accepted even if its clock went back. A client with no clock connects about 30 s after a reboot. Meant for closed networks with no Internet (caveats in the [advanced guide §6](guide-advanced.md#6-time-tai64n))

A device that only waits for others to connect (a server) does not need the clock.

## 9. Boards other than the ESP32

| Board | Example | Differences |
|---|---|---|
| CH32V307 and others with Ethernet | `Ch32EthernetClient` | `ETH.begin()` instead of Wi-Fi; UDP is `NetworkUDP` |
| Raspberry Pi Pico W / Pico 2 W | `PicoWClient` | clock set with `NTP.begin()`; build-tested only for now |

The sketch is put together the same way on all of them.

## 10. What to try next

### Make the ESP32 the server

In the `Esp32Server` example the ESP32 is the WireGuard server. A PC or a phone connects to it and reaches a TCP echo server (port 7) through the tunnel.

The ESP32 only needs each peer's public key and tunnel address, not its IP address: it learns that from the packets that arrive.

The other side (a PC, or the official app on a phone) looks like this:

```ini
[Interface]
PrivateKey = <its private key>
Address = 10.9.0.2/24

[Peer]
PublicKey = <the ESP32's public key (printed on the serial port at start-up)>
Endpoint = <the ESP32's LAN address>:51820
AllowedIPs = 10.9.0.1/32
PersistentKeepalive = 25
```

Once connected, run `nc 10.9.0.1 7` on the PC. What you type comes back.

### MQTT and HTTP

TinyWG's `wg0` is an ordinary network interface. MQTT clients such as `PubSubClient`, and HTTP clients and servers, go through the tunnel when you point them at a tunnel address.

## 11. Troubleshooting

First print the counters:

```cpp
tinywg::Stats s = wg.stats();
Serial.printf("init=%u done=%u badmac=%u unknown=%u hsfail=%u stale=%u notallowed=%u\n",
              (unsigned)s.handshakesInitiated, (unsigned)s.handshakesCompleted, (unsigned)s.rxBadMac1,
              (unsigned)s.rxUnknownPeer, (unsigned)s.rxHandshakeFail, (unsigned)s.rxStaleTimestamp,
              (unsigned)s.rxNotAllowed);
```

| Symptom | Likely cause |
|---|---|
| `wg.begin()` returns false | The private key string is malformed (44 characters of Base64?), or the chip has no random source (see the advanced guide) |
| `addPeer()` returns -1 | Called before `begin()`; the public key string is malformed; the same key was added twice; the peer table is full (4 by default) |
| `init` grows but `done` does not | Nothing reaches the PC, or the PC refuses it. Check the Endpoint address and port, the PC's firewall (`51820/udp`) and `PublicKey` under the PC's `[Peer]` |
| `sudo wg show` on the PC shows nothing | The ESP32's packets do not reach the PC. Same LAN? Is the Endpoint the PC's address? |
| Works on the first boot only | The client's clock went back (§8). The other side silently drops handshakes with an old time (`stale` grows) |
| `badmac` grows | The other side has the wrong public key for this device |
| `unknown` grows | A peer whose public key is not registered tried to connect (the key given to `addPeer()` is wrong) |
| `notallowed` grows | The peer's tunnel address is outside what `addAllowedIp()` allows |
| Connected, but ping gets no answer | The PC's firewall blocks `wg0` (`sudo ufw allow in on wg0`) |

## 12. Glossary

| Term | Meaning |
|---|---|
| Peer | One device at the other end of the tunnel |
| Endpoint | The peer's "outer" IP address and port, where the encrypted UDP goes |
| AllowedIPs | The addresses sent to that peer, and the source addresses accepted from it |
| Handshake | The exchange that sets up session keys; repeated every 2 minutes while traffic flows |
| Keepalive | An empty packet sent when there is nothing else, so NATs and routers keep the path open |
| MTU | The largest packet inside the tunnel (1420 bytes by default) |

The finer settings and how it all works are in the [advanced guide](guide-advanced.md).
