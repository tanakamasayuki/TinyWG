# Advanced guide

[日本語](guide-advanced.ja.md) | [Beginner's guide](guide-beginner.md) | [API reference](api.md) | [README](../README.md) | [Specification](../SPEC.md)

How TinyWG is built, what the API does in detail, and what to watch for in the field. It assumes you have been through the [beginner's guide](guide-beginner.md).

## 1. Structure

```
Application (WiFiClient, MQTT, HTTP, raw IPv4 ...)
   │
   ├─ TinyWG      lwIP network interface wg0 (ESP32 / Pico W / CH32V)
   ├─ TinyWGUdp   over any Arduino UDP; raw IPv4 packets inside
   │
tinywg::Device    protocol engine (handshake, sessions, timers, AllowedIPs)
   │              no dependency on lwIP or Arduino
Crypto            Monocypher (X25519, ChaCha20-Poly1305), BLAKE2s (RFC 7693)
```

| Front end | When to use it | Called from `loop()` |
|---|---|---|
| `TinyWG` | The core has lwIP and existing socket code should go through the tunnel unchanged | nothing (it runs inside lwIP) |
| `TinyWGUdp` | No lwIP, or you want to build and parse the inner packets yourself | `wg.update()` |
| `tinywg::Device` | The outer UDP is not an Arduino `UDP` (a cellular modem's AT commands, for example) | `receiveOuter()` and `tick()` |

None of them allocates memory dynamically; every buffer lives inside the object.

## 2. Execution context and locking

In `TinyWG`, all three of these run in lwIP's context:

- outer UDP receive
- output from `wg0`
- a timer every 100 ms (`sys_timeout`)

The public methods called from the sketch serialise with lwIP in a per-core way:

| Core | Lock | Note |
|---|---|---|
| ESP32 | `LOCK_TCPIP_CORE()` | Calling a public method from inside an lwIP callback (some `WiFi.onEvent` handlers, raw API callbacks) deadlocks |
| arduino-pico | `LWIPMutex` | In FreeRTOS mode `TinyWG` is not available (using it stops the build with a message saying so). `TinyWGUdp`, `Device` and `TinyWGTimeServer` work |
| CH32V | none | lwIP runs only after each `loop()` and inside `delay()`. Blocking `loop()` for long stops handshakes and timers too |

When you use the engine directly through `wg.device()` on the ESP32, wrap the calls in `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` yourself.

`TinyWGUdp` and `Device` assume no threads: make every call from the same task.

### Replacing the outer UDP

A W5500 on Arduino's `Ethernet` library, or a modem such as the SIM7080G driven by AT commands, runs TCP/IP inside the module. That is outside lwIP, so as it is, `wg0`'s outer traffic never leaves through the module.

With `setOuterUdp()`, `wg0` stays in lwIP and only the outer UDP goes to the module's `UDP`. Inside the tunnel, `NetworkClient`, MQTT and other code on lwIP work as they are.

```cpp
EthernetUDP outer;
TinyWG wg;

wg.setOuterUdp(&outer);                                     // before begin()
wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, 51820);      // opens outer on 51820
```

By default the library drives the outer I/O itself.

| Core | Drives the outer side | Notes |
|---|---|---|
| ESP32 | A FreeRTOS task of its own (priority 2, every 1 ms) | The tunnel keeps going while `loop()` waits in a socket write or an MQTT connect. In exchange, the sketch must not use the same module at the same time (an `EthernetClient` outside the tunnel, say) |
| CH32V | lwIP's timer (every 1 ms) | lwIP's timers run in `loop()`'s flow, so they run inside blocking calls too |
| arduino-pico | `wg.update()` in `loop()` | Not driven automatically |

When the sketch also uses the module directly, call `wg.setOuterUdp(&outer, true)` and `wg.update()` from `loop()`. Then the tunnel stops whenever `loop()` does. Calls that wait for the other side, such as a socket write, cannot be used (the ESP32's `WiFiClient::write()` waits up to 10 s for ACKs, and traffic jams meanwhile). `update()` is harmless to call in every mode.

- Packets leaving `wg0` wait in a queue (8 deep) until the outer driver hands them to the module. What overflows is dropped, as UDP would be
- The outer side does not go through lwIP, so there is no "do not send when the outer destination routes into `wg0`" loop check. Without Wi-Fi, `wg0` can even be the default route
- The tests check it with an ESP32 `WiFiUDP` as the outer side. It has not been tried on a W5500 or a SIM7080G yet

## 3. Order of configuration, and what lasts

```cpp
wg.setRandom(...);       // optional; before begin() (begin() uses it)
wg.setWallClock(...);    // optional; any time
wg.setMtu(1280);         // optional; any time
wg.begin(ip, mask, privateKey, listenPort);
int p = wg.addPeer(publicKey, psk);   // after begin()
wg.addAllowedIp(p, ...);
wg.setEndpoint(p, ...);               // optional
wg.setPersistentKeepalive(p, 25);     // optional
```

- `begin()` clears the peer table and the counters every time. To change keys, start again from `begin()` and add the peers again
- `addPeer()` returns -1 before `begin()`. It computes one X25519, so each peer takes about 18 ms on an ESP32 and 24 ms on a CH32V307
- An index freed by `removePeer()` is reused by the next `addPeer()`; mind code that keeps indexes around
- `setRandom()`, `setWallClock()` and `setMtu()` survive `begin()` / `end()`
- `setPersistentKeepalive()` sends one keepalive right away, as Linux does. With no session and an Endpoint, that starts a handshake

### From wg-quick text

`begin()` and the peers can come from configuration text in the format of `wg-quick` and the official apps, in one call.

```cpp
static const char *kConfig = R"(
[Interface]
PrivateKey = <this device's private key>
Address = 10.9.0.2/24
ListenPort = 51820

[Peer]
PublicKey = <the server's public key>
AllowedIPs = 10.9.0.0/24
Endpoint = 192.168.1.10:51820
PersistentKeepalive = 25
)";

int errorLine;
if (!wg.beginConfig(kConfig, &errorLine)) Serial.printf("config error at line %d\n", errorLine);
```

- Read: `PrivateKey`, `Address` (required by `TinyWG`), `ListenPort` and `MTU` under `[Interface]`; `PublicKey`, `PresharedKey`, `AllowedIPs`, `Endpoint` and `PersistentKeepalive` under `[Peer]`
- IPv6 addresses and AllowedIPs are skipped; `DNS`, `Table`, `PostUp` and the like are ignored. A configuration made by a phone app can be passed as it is
- `Endpoint` must be an IPv4 address and port. Resolve a host name in the sketch, as in §4
- On failure it returns false with the offending line in `errorLine` (0: no `PrivateKey` or `Address`, or `begin()` failed). Whatever was set up by then is undone with `end()`
- With `TinyWGUdp` it is `wg.beginConfig(udp, kConfig, &errorLine)` (no `Address` needed)
- A sketch that never calls it does not link the parser (calling it costs about 3.4 KB of flash on an ESP32)

## 4. Routing

### AllowedIPs (cryptokey routing)

The ranges given to `addAllowedIp()` mean two things:

- **Sending:** the destination is looked up by longest prefix to choose the peer
- **Receiving:** the source of a decrypted packet must fall in that peer's ranges; otherwise it is dropped and counted in `rxNotAllowed`

Each peer has up to `TINYWG_MAX_ALLOWED_IPS` (4 by default) entries.

### What lwIP sends into wg0

lwIP knows only each interface's subnet and a default route. Only traffic for the address and mask given to `begin()` (e.g. `10.9.0.0/24`) goes into `wg0`.

An AllowedIPs entry of `192.168.50.0/24` does nothing unless lwIP routes that destination to `wg0`. When several networks lie behind the tunnel, the simplest way is a wider mask in `begin()`.

You can make `wg0` the default route with `netif_set_default(wg.netif())`. But if the outer UDP to the Endpoint would itself enter the tunnel, TinyWG does not send it, to avoid a loop. So `wg0` can be the default route only when the Endpoint is on a directly connected subnet (the same LAN).

### NAPT (ESP32)

`wg.enableNapt()` translates traffic that peers send into `wg0` and that leaves by another interface (Wi-Fi, for example) to the ESP32's LAN address. This lets a peer come in through the tunnel and go on to the home LAN or the Internet.

On the peer, add the LAN to AllowedIPs:

```ini
[Peer]
PublicKey = <the ESP32's public key>
Endpoint = <an address that reaches the ESP32>:51820
AllowedIPs = 10.9.0.0/24, 192.168.1.0/24
```

On the ESP32, the peer's tunnel address (`10.9.0.2/32` or similar) is enough in AllowedIPs. NAPT maps replies from the LAN back to the peer's address before they enter `wg0`.

### Roaming

Every authenticated packet updates the peer's Endpoint to its source. When the peer's address or port changes (a NAT rebinding, a switch of network), one packet from it is enough to follow.

The same holds the other way: when this device's address changes, anything it sends (persistent keepalive included) lets the peer follow.

### The endpoint by host name

TinyWG does no DNS. To name the server by host name, resolve it in the sketch and pass the address to `setEndpoint()`.

```cpp
static const char *kServerHost = "vpn.example.com";

static void resolveServer()
{
    IPAddress ip;
    if (Network.hostByName(kServerHost, ip)) wg.setEndpoint(server, ip, 51820);   // Pico W: WiFi.hostByName()
}

void setup()
{
    // ... after wg.begin() and wg.addPeer()
    resolveServer();
}

void loop()
{
    // When the server's address can change (dynamic DNS and the like), resolve again now and then
    static uint32_t last = millis();
    if (millis() - last >= 10 * 60 * 1000) {
        last = millis();
        resolveServer();
    }
}
```

- Authenticated packets from the server update the Endpoint as well. Resolving again matters when this side sends and gets no answer (the server moved and nothing reaches it). Resolving only when `lastHandshakeMs` in `peerStatus()` grows old works too
- `hostByName()` waits for the DNS answer and can block for seconds. Do not call it inside an lwIP callback
- With a module's UDP through `setOuterUdp()`, use the module's DNS (`DNSClient` with the W5500 `Ethernet` library)

## 5. Keys

### Generate and store the key on the device (ESP32)

Instead of writing the private key into the source, generate it on first boot and keep it in NVS. Print the public key and register it with the peer.

```cpp
#include <Preferences.h>

uint8_t key[32];
Preferences prefs;
prefs.begin("tinywg");
if (prefs.getBytes("key", key, sizeof(key)) != sizeof(key)) {
    tinywg::generatePrivateKey(key);   // the default random source (esp_fill_random)
    prefs.putBytes("key", key, sizeof(key));
}
prefs.end();
wg.begin(ip, mask, key, 51820);
memset(key, 0, sizeof(key));

char pub[45];
wg.publicKey(pub);
Serial.println(pub);   // goes into the peer's [Peer] PublicKey
```

- NVS and flash can be read out as they are. For devices others can get their hands on, consider the ESP32's flash encryption and NVS encryption
- `tinywg::keyFromBase64()`, `keyToBase64()` and `derivePublicKey()` convert keys

### Preshared key (PSK)

Pass a key made with `wg genpsk` as the second argument of `addPeer(publicKey, psk)`, and the same value as the peer's `PresharedKey`. If they differ, the handshake fails and `rxHandshakeFail` grows.

## 6. Time (TAI64N)

The side that starts a handshake sends a TAI64N timestamp. The receiver accepts only a value greater than the last one it took from that peer.

Within one boot TinyWG keeps the values it sends increasing even if the clock steps back. What needs care is across reboots.

| Method | When |
|---|---|
| NTP (the default, `gettimeofday()`) | The device can get online before `begin()` |
| RTC | There is a battery-backed RTC; pass a function that reads it to `setWallClock()` |
| Boot counter | Neither NTP nor an RTC (example below) |

Counting boots in non-volatile storage instead of a clock (ESP32):

```cpp
#include <Preferences.h>
#include <esp_timer.h>

static uint32_t bootCount;

// Boot count in the upper half, seconds since boot below: always greater than any
// real time up to 2106 (which fits in 32 bits).
static void bootClock(uint64_t *seconds, uint32_t *nanoseconds)
{
    uint64_t us = esp_timer_get_time();
    *seconds = ((uint64_t)bootCount << 32) + us / 1000000;
    *nanoseconds = (uint32_t)(us % 1000000) * 1000;
}

void setup()
{
    Preferences prefs;
    prefs.begin("tinywg");
    bootCount = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", bootCount);
    prefs.end();
    wg.setWallClock(bootClock);
    // ...
}
```

Once you switch to this, the peer remembers the large value, and going back to real NTP time is refused. To go back, remove and add the peer again on the other side.

A device that only answers handshakes (a responder) does not use the clock.

### Accepting clients with no clock

When TinyWG is the side that accepts connections, `setTimestampReset()` makes it forget the timestamp record of a peer it has heard nothing from for a while. Use it on closed networks and the like, where the clients have neither NTP nor an RTC.

```cpp
wg.setTimestampReset(30);   // a peer silent for 30 s (no response, no data) is accepted with an older time
```

- The default is 0: never forget, as standard WireGuard
- Only authenticated handshake responses and data (keepalives included) count as heard. An initiation does not, as it may be a replay
- A rebooted client connects once this many seconds have passed since it last talked. Clients resend the handshake every 5 s for up to 90 s, so a value under 90 lets them connect within that
- With persistent keepalive, make it longer than the keepalive interval. Shorter, and a healthy peer counts as silent between keepalives

The price is a wider opening for replays. A recorded initiation sent again is accepted for a peer while it is silent. That can briefly redirect traffic for that peer or break a handshake in progress, stopping traffic for a while. It cannot leak keys, impersonate the peer or inject data. A peer that is talking still has replays refused as before.

Linux and the official apps have no such setting: when they are the server, set the client's clock.

### Simple NTP server (`TinyWGTimeServer`)

It answers NTP (SNTP) on the WG server's LAN address with the device's own clock, so clients can use the WG endpoint address as their NTP server too. Which source to use on which network: [beginner's guide §8.2](guide-beginner.md#82-where-to-get-the-time-from).

```cpp
WiFiUDP ntpUdp;               // a UDP object for NTP only, not the one TinyWGUdp uses
TinyWGTimeServer timeServer;

void setup()
{
    // ...
    configTime(0, 0, "pool.ntp.org");   // real time when online, otherwise counting from 1970
    timeServer.begin(ntpUdp);           // UDP port 123
}

void loop()
{
    timeServer.update();                // answers the requests that arrived
    // ...
}
```

- The time served is `gettimeofday()` by default (the clock TinyWG uses for handshakes). `setClock()` replaces it, e.g. with a function that reads an RTC
- Replies carry stratum 10 and reference ID `LOCL` (as chrony's `local stratum 10`). They never set the leap warning (the "unsynchronised" mark), even when the clock is not set, as some clients refuse such replies
- Call `update()` from `loop()`, and from inside anything that blocks for long, so replies are not late
- It costs nothing when unused: with no instance, neither flash nor RAM grows (checked on ESP32 and CH32V307)

NTP replies are not authenticated. An attacker on the same network can answer with a time far in the future. The client's handshake timestamps then jump ahead, and once its clock is right again its handshakes are refused. With `setTimestampReset()` on a TinyWG server, that clears after the set time. A Linux server refuses until its WireGuard restarts.

## 7. Random numbers

Key generation and the ephemeral key of every handshake need cryptographic random numbers.

| Core | Default source |
|---|---|
| ESP32 | `esp_fill_random()` (hardware RNG) |
| RP2040 / RP2350 | `get_rand_64()` |
| CH32V | TRNG (V307 and others). Series without one (V208 and others) have no source |
| host | `/dev/urandom` |

With no source, `begin()` returns false. Supply one with `wg.setRandom(fn)` before `begin()`. `fn` is `bool fn(uint8_t *out, size_t len)` and must return cryptographically secure bytes; `micros()` or `rand()` will not do.

## 8. MTU and memory

### MTU

- The inner MTU is 1420 bytes by default. Outer packets are 60 bytes larger than inner ones (IPv4 20 + UDP 8 + WireGuard header 16 + tag 16)
- When the outer path is smaller than 1500 (PPPoE, LTE, inside another tunnel), lower it with `setMtu()`: from 576 up to `TINYWG_MTU`, at any time
- With `TinyWG`, `wg0`'s MTU changes as well; lwIP then sizes TCP segments to fit and fragments UDP
- A packet larger than the MTU is not sent: the send returns `SendResult::Invalid`

### Build-time settings

Override with `-D` (`src/tinywg/wg_config.h`). With `arduino-cli`: `--build-property "compiler.cpp.extra_flags=-DTINYWG_MAX_PEERS=8"`.

| Macro | Default | Effect on RAM |
|---|---|---|
| `TINYWG_MAX_PEERS` | 4 | about 0.7 KB per peer |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | a few bytes per entry |
| `TINYWG_MTU` | 1420 | sizes the send buffer (about MTU bytes) |
| `TINYWG_REPLAY_WINDOW` | 128 | window in bits; a multiple of 32, at least 64 |

With the defaults `Device` is about 4.5 KB. All of `TinyWG` adds 26.9 KB of flash and 5.4 KB of static RAM on an ESP32. Details are in [SPEC.md](../SPEC.md) §5.

## 9. Timers and behaviour

The values are those of the WireGuard whitepaper, §6.

| Constant | Value | Behaviour |
|---|---|---|
| REKEY_AFTER_TIME | 120 s | The side that started the session begins a new handshake when it sends after this |
| REJECT_AFTER_TIME | 180 s | No sending or receiving on an older session |
| REKEY_TIMEOUT | 5 s | Retransmit a handshake that got no response |
| REKEY_ATTEMPT_TIME | 90 s | Give up retransmitting; the next send tries again |
| KEEPALIVE_TIMEOUT | 10 s | After receiving data, send an empty packet if nothing was sent for 10 s (passive keepalive) |
| Key zeroing | 540 s | Session keys are erased this long after the last handshake |

At most 50 handshake initiations per second are accepted from one peer; the rest are dropped and counted in `rxFlood`.

The last packet sent while there is no session waits until the handshake completes and is then sent (`SendResult::Queued`). By default it waits in the send buffer, costing no RAM; in exchange, sending anything to another peer meanwhile loses it. A client with one peer never sees this. On a gateway with several peers, give `setStagingBuffer()` MTU bytes and it is kept.

```cpp
static uint8_t staging[1420];
wg.setStagingBuffer(staging, sizeof(staging));
```

## 10. Monitoring and diagnosis

### `peerStatus()`

| Field | Meaning |
|---|---|
| `sessionUp` | A session that can send right now |
| `handshakeInFlight` | An initiation waiting for its response |
| `hasHandshake` | A handshake has completed at least once |
| `lastHandshakeMs` | `millis()` of the latest completed handshake |
| `endpoint` | Where packets go now (roaming changes it) |
| `rxBytes` / `txBytes` | Outer UDP bytes (WireGuard header and tag included) |

### `stats()` (dropped packets, device-wide)

| Field | Usual cause |
|---|---|
| `rxShort` | Too short, or an unknown message type |
| `rxBadMac1` | The peer has the wrong public key for this device |
| `rxHandshakeFail` | A handshake that failed to decrypt or authenticate (a PSK mismatch, for example) |
| `rxUnknownPeer` | A handshake from a public key that is not registered |
| `rxStaleTimestamp` | A handshake whose time is not newer than the last (a replay, or the peer's clock went back) |
| `rxFlood` | Handshakes beyond 50 per second |
| `rxNoSession` | Data for an unknown session index (seen right after this device restarts, for example) |
| `rxDecryptFail` | Data that failed authentication (tampered, or an old key) |
| `rxReplay` | A counter outside the window or already seen |
| `rxNotAllowed` | The source is not in the peer's AllowedIPs |
| `rxNotIpv4` | The inner packet is not well-formed IPv4 |
| `handshakesInitiated` / `Completed` / `Responded` | Handshakes started / completed / answered |
| `cookiesSent` / `rxUnderLoad` | Cookies a `CookieGuard` sent under load / handshakes it dropped before the expensive part (§13) |

## 11. `TinyWGUdp`: handling inner packets yourself

The sketch handles the inner IPv4 packets directly, without lwIP. This one answers ping inside the tunnel:

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>

WiFiUDP udp;
TinyWGUdp wg;

static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// packet may be modified: here an ICMP echo request is turned into the reply and sent back
static void onPacket(int peer, uint8_t *p, size_t len, void *)
{
    size_t hl = (p[0] & 0x0F) * 4;
    if (p[9] != 1 || p[hl] != 8) return;          // ICMP echo requests only
    p[hl] = 0;                                    // echo reply
    p[hl + 2] = p[hl + 3] = 0;
    uint16_t c = checksum(p + hl, len - hl);
    p[hl + 2] = c >> 8;
    p[hl + 3] = c;
    uint8_t tmp[4];                               // swap source and destination
    memcpy(tmp, p + 12, 4);
    memcpy(p + 12, p + 16, 4);
    memcpy(p + 16, tmp, 4);
    p[10] = p[11] = 0;
    c = checksum(p, hl);
    p[10] = c >> 8;
    p[11] = c;
    wg.send(p, len);
}

void setup()
{
    // ... join Wi-Fi
    wg.begin(udp, 51820, "<private key>");
    int peer = wg.addPeer("<peer public key>");
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 1), 32);
    wg.onPacket(onPacket);
}

void loop()
{
    wg.update();   // receive and timers; do not leave more than 100 ms between calls
}
```

- Calling `wg.send()` from inside `onPacket` is fine
- `send()` picks the peer by AllowedIPs. There is no notion of our own tunnel address: the source address is whatever the packet says

## 12. Using `tinywg::Device` directly

When you bring your own outer UDP, implement `tinywg::Transport` and hand it to `Device`.

```cpp
#include <TinyWG.h>

class ModemTransport : public tinywg::Transport {
public:
    void sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len) override
    {
        // send data over UDP to to.addr[0..3]:to.port
    }
    void deliverInner(int peer, uint8_t *packet, size_t len) override
    {
        // a decrypted inner IPv4 packet; packet may be modified
    }
};

tinywg::Device dev;
ModemTransport transport;
uint8_t rx[16 + TINYWG_MTU + 32];

void setup()
{
    uint8_t priv[32], pub[32];
    tinywg::keyFromBase64(priv, "<private key>");
    tinywg::keyFromBase64(pub, "<peer public key>");
    dev.begin(priv, &transport);
    int p = dev.addPeer(pub);
    const uint8_t net[4] = {10, 9, 0, 0};
    dev.addAllowedIp(p, net, 24);
}

void loop()
{
    // for each outer UDP datagram received:
    //   tinywg::Endpoint from = {{a, b, c, d}, port};
    //   dev.receiveOuter(rx, len, from, millis());   // rx is decrypted in place
    // to send an inner packet:
    //   dev.sendInner(packet, len, millis());
    dev.tick(millis());   // every 100 ms or so
}
```

Time is a 32-bit millisecond count like `millis()`; the wrap after 49.7 days is handled.

## 13. Security notes

- **Attach a `CookieGuard` against handshake floods.** Anyone who knows the public key can send handshakes with a valid mac1 in bulk, each costing expensive work (about 73 ms on an ESP32). On a server exposed directly to the Internet, attach one:

  ```cpp
  static tinywg::CookieGuard guard;   // default: under load above 4 a second; 2 a second per source
  wg.setCookieGuard(&guard);
  ```

  Under load it answers with a cookie before the expensive part, and handles only senders that really own their source address (they resend with a mac2 made from the cookie), at a limited rate per source. Floods with forged sources are dropped cheaply. Receivers of the cookie (TinyWG, Linux and others) cope by themselves on their retransmission 5 s later. A sketch that does not attach one does not link this code (attached: about 1 KB of flash and 150 B of RAM on an ESP32). Even without it, handshakes from anyone who does not know this device's public key are dropped at the mac1 check, and one peer gets at most 50 a second
- **Key storage:** the private key sits in flash or NVS in the clear (§5)
- **IPv4 only**, inside and outside

## 14. Tests and measurements

- The tests and how to run them: [tests/README.md](../tests/README.md)
- Interoperability with the Linux kernel WireGuard is checked by automated tests
- On-board measurements (X25519, handshake, AEAD speed, ping, throughput, long runs, Wi-Fi outages): [SPEC.md](../SPEC.md) §5.2
