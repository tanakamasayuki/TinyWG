# Advanced guide

[日本語](guide-advanced.ja.md) | [Beginner's guide](guide-beginner.md) | [README](../README.md) | [Specification](../SPEC.md)

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
| arduino-pico | `LWIPMutex` | FreeRTOS mode is not supported (it is a compile error) |
| CH32V | none | lwIP runs only after each `loop()` and inside `delay()`. Blocking `loop()` for long stops handshakes and timers too |

When you use the engine directly through `wg.device()` on the ESP32, wrap the calls in `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` yourself.

`TinyWGUdp` and `Device` assume no threads: make every call from the same task.

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
| `TINYWG_MAX_PEERS` | 4 | about 0.8 KB per peer |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | a few bytes per entry |
| `TINYWG_MTU` | 1420 | sizes the send buffer, the staged packet and `TinyWG`'s scratch buffer (about MTU bytes each) |
| `TINYWG_REPLAY_WINDOW` | 128 | window in bits; a multiple of 32, at least 64 |
| `TINYWG_STAGED_PACKET` | 1 | 0: do not hold a packet sent during a handshake (saves about MTU bytes) |

With the defaults `Device` is about 6.3 KB. All of `TinyWG` adds 25.6 KB of flash and 8.2 KB of static RAM on an ESP32. Details are in [SPEC.md](../SPEC.md) §5.

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

With `TINYWG_STAGED_PACKET` at 1, the last packet sent while there is no session is held and sent once the handshake completes (`SendResult::Queued`).

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

- **No Cookie Reply is sent.** Asking peers for a cookie under load (the DoS defence) is not implemented yet; receiving one is. A server exposed directly to the Internet can be flooded with handshake work (about 73 ms each on an ESP32). There is a per-peer limit (50 per second), and handshakes from anyone who does not know this device's public key are dropped at the mac1 check, before the expensive part
- **Key storage:** the private key sits in flash or NVS in the clear (§5)
- **IPv4 only**, inside and outside

## 14. Tests and measurements

- The tests and how to run them: [tests/README.ja.md](../tests/README.ja.md)
- Interoperability with the Linux kernel WireGuard is checked by automated tests
- On-board measurements (X25519, handshake, AEAD speed, ping, throughput, long runs, Wi-Fi outages): [SPEC.md](../SPEC.md) §5.2
