# API reference

[日本語](api.ja.md) | [Beginner's guide](guide-beginner.md) | [Advanced guide](guide-advanced.md)

`#include <TinyWG.h>` brings in everything listed here.

- [`TinyWGBase`](#tinywgbase): methods shared by `TinyWG` and `TinyWGUdp`
- [`TinyWG`](#tinywg): the lwIP network interface `wg0`
- [`TinyWGUdp`](#tinywgudp): raw IPv4 packets over any Arduino `UDP`
- [`TinyWGTimeServer`](#tinywgtimeserver): an optional simple NTP server
- [`tinywg::CookieGuard`](#tinywgcookieguard): optional cookie replies against handshake floods
- [`tinywg::Device`](#tinywgdevice): the protocol engine
- [Types](#types): `SendResult`, `PeerStatus`, `Stats`, `Endpoint`, function types
- [Key functions](#key-functions)

Key strings are always the 44-character Base64 printed by `wg genkey` / `wg pubkey` / `wg genpsk`.

## TinyWGBase

### Peers

| Method | Returns | Description |
|---|---|---|
| `int addPeer(const char *publicKeyBase64, const char *presharedKeyBase64 = nullptr)` | peer index, or -1 | Adds a peer. Call after `begin()`. -1 before `begin()`, for a malformed key string, a public key already added, or a full table (`TINYWG_MAX_PEERS`). Computes one X25519 (about 18 ms on an ESP32) |
| `int addPeer(const uint8_t publicKey[32], const uint8_t *presharedKey = nullptr)` | same | Keys as bytes |
| `bool removePeer(int peer)` | success | Removes the peer and wipes its keys. The index is reused by the next `addPeer()` |
| `bool addAllowedIp(int peer, const IPAddress &address, uint8_t cidr)` | success | Adds one AllowedIPs entry (e.g. `IPAddress(10, 9, 0, 0), 24`). Up to `TINYWG_MAX_ALLOWED_IPS` per peer; `cidr` 0 to 32 |
| `bool setEndpoint(int peer, const IPAddress &address, uint16_t port)` | success | Where handshakes go. Not needed for a peer that only connects to us. Every authenticated packet overwrites it with its source |
| `bool setPersistentKeepalive(int peer, uint16_t seconds)` | success | Keepalive interval in seconds; 0 stops it. Sends one right away; with no session and an Endpoint, that starts a handshake |
| `bool connect(int peer)` | whether a handshake was sent | Starts a handshake now; normally the first packet does. false with no Endpoint, within 5 s of the previous start, or for a bad index |

### MTU

| Method | Returns | Description |
|---|---|---|
| `virtual bool setMtu(uint16_t mtu)` | success | Inner MTU, 576 to `TINYWG_MTU`. Out of range: false, nothing changes. Any time. With `TinyWG`, `wg0`'s MTU changes too |
| `uint16_t mtu() const` | the MTU in use | |

### Packet waiting for a handshake

| Method | Returns | Description |
|---|---|---|
| `bool setStagingBuffer(uint8_t *buffer, size_t size)` | success | Where the last packet sent with no session waits for the handshake. The default (`nullptr`) is the send buffer: no RAM, but sending to another peer meanwhile loses it. A buffer of MTU bytes keeps it (gateways with several peers). false when `size` is under 576. A packet larger than `size` waits in the send buffer |

### Handshake floods

| Method | Description |
|---|---|
| `void setCookieGuard(tinywg::HandshakeGuard *guard)` | Attaches a `tinywg::CookieGuard`, which answers with cookie replies under load. `nullptr` removes it. Not linked unless used. [Advanced guide §13](guide-advanced.md#13-security-notes) |

### Timestamp check

| Method | Description |
|---|---|
| `void setTimestampReset(uint16_t seconds)` | Forgets a peer's last handshake timestamp once neither a response nor data has come from it for `seconds`, so a client whose clock went back (rebooted with no NTP or RTC) can connect. 0 (the default) never forgets, as standard WireGuard. Any time; kept across `begin()`. What it means for replays: [advanced guide §6](guide-advanced.md#6-time-tai64n) |

### State

| Method | Returns | Description |
|---|---|---|
| `bool peerUp(int peer)` | whether a session can send right now | `sessionUp` of `peerStatus()` |
| `bool peerStatus(int peer, tinywg::PeerStatus *out)` | whether the index is valid | Fills `out` ([`PeerStatus`](#peerstatus)) |
| `tinywg::Stats stats()` | counters | Dropped packets and handshakes, device-wide ([`Stats`](#stats)). Reset by `begin()` |
| `void publicKey(char out[45]) const` | | This device's public key in Base64 (44 characters + NUL), for the peer's `[Peer] PublicKey` |

### Replacing the platform hooks

| Method | Description |
|---|---|
| `void setRandom(tinywg::RandomFn fn)` | Replaces the cryptographic random source. Call before `begin()`. Required on cores with no default source |
| `void setWallClock(tinywg::WallClockFn fn)` | Replaces the clock used for TAI64N. The default is `gettimeofday()` |
| `tinywg::Device &device()` | The engine itself, for what the wrapper does not expose. With `TinyWG` on an ESP32, wrap calls in `LOCK_TCPIP_CORE()` |

## TinyWG

Derives from `TinyWGBase`. Available on cores with lwIP (ESP32, arduino-pico, CH32V with the Network library).

| Method | Returns | Description |
|---|---|---|
| `bool begin(const IPAddress &localIp, const IPAddress &subnet, const char *privateKeyBase64, uint16_t listenPort = 51820)` | success | Brings up `wg0` and opens the outer UDP port. Traffic for the `localIp` / `subnet` range goes into `wg0`. `listenPort` 0 picks a free port (for clients). Clears the peer table and the counters. false for a malformed key string, no random source, or a port that cannot be opened |
| `bool begin(const IPAddress &localIp, const IPAddress &subnet, const uint8_t privateKey[32], uint16_t listenPort = 51820)` | same | Key as bytes |
| `void end()` | | Closes `wg0` and the port and wipes the keys |
| `bool beginConfig(const char *text, int *errorLine = nullptr)` | success | `begin()` and the peers from wg-quick text. The failing line goes to `errorLine` (0: no `PrivateKey` / `Address`, or `begin()` failed). Not linked unless called. [Advanced guide §3](guide-advanced.md#from-wg-quick-text) |
| `bool setMtu(uint16_t mtu)` | success | `TinyWGBase::setMtu()`, and `wg0`'s MTU as well |
| `struct netif *netif()` | `wg0` | For routing or NAPT set up by the sketch |
| `bool enableNapt(bool enable = true)` | success | ESP32 only (when `IP_NAPT` is on). NAPT for traffic that peers send into `wg0` and that leaves by another interface. Call after `begin()` |
| `bool setOuterUdp(UDP *udp, bool pollFromLoop = false)` | success | Moves the outer UDP to any Arduino `UDP` (a W5500's `EthernetUDP`, a modem's AT socket and so on); `wg0` stays in lwIP. Call before `begin()`, which opens `udp`. `nullptr` goes back to lwIP's UDP. false while running. By default the library drives the outer I/O itself (ESP32: a task of its own; CH32V: lwIP's timer). With `pollFromLoop = true`, and on arduino-pico, `update()` drives it. [Advanced guide §2](guide-advanced.md#replacing-the-outer-udp) |
| `void update()` | | With `setOuterUdp(udp, true)` and on arduino-pico: reads the outer datagrams that arrived and sends the queued ones. Call from `loop()` often. Otherwise it does nothing |

Normally nothing is called from `loop()`: receiving and the timers run inside lwIP. On an ESP32, do not call the public methods from inside an lwIP callback (it deadlocks).

## TinyWGUdp

Derives from `TinyWGBase`. Works on any core.

| Method | Returns | Description |
|---|---|---|
| `bool begin(UDP &udp, uint16_t listenPort, const char *privateKeyBase64)` | success | Opens `udp` on `listenPort` and starts. Clears the peer table and the counters |
| `bool begin(UDP &udp, uint16_t listenPort, const uint8_t privateKey[32])` | same | Key as bytes |
| `void end()` | | Closes `udp` and wipes the keys |
| `bool beginConfig(UDP &udp, const char *text, int *errorLine = nullptr)` | success | As `TinyWG::beginConfig()`; `Address` is not used |
| `void onPacket(PacketHandler handler, void *context = nullptr)` | | Registers the function that receives decrypted inner IPv4 packets |
| `tinywg::SendResult send(const uint8_t *packet, size_t len)` | [`SendResult`](#sendresult) | Sends one inner IPv4 packet; the peer is chosen by destination and AllowedIPs |
| `void update()` | | Handles received datagrams and runs the timers. Call from `loop()`, no more than 100 ms apart |

```cpp
typedef void (*PacketHandler)(int peer, uint8_t *packet, size_t len, void *context);
```

`packet` may be modified, and the handler may call `send()`.

## TinyWGTimeServer

A small server that answers NTP (SNTP) with the device's own clock. Works over any core's `UDP`. How to use it: [advanced guide §6](guide-advanced.md#simple-ntp-server-tinywgtimeserver).

| Method | Returns | Description |
|---|---|---|
| `bool begin(UDP &udp, uint16_t port = 123)` | success | Opens `udp` on `port`. Give it a `UDP` of its own |
| `void end()` | | Closes `udp` |
| `void update()` | | Answers the requests that arrived. Call from `loop()` |
| `void setClock(tinywg::WallClockFn fn)` | | Replaces the clock served. The default is `gettimeofday()` |

## tinywg::CookieGuard

Available through `#include <TinyWG.h>` (`tinywg/wg_cookie.h`).

| Method | Description |
|---|---|
| `CookieGuard(uint8_t handshakesPerSecond = 4, uint8_t perSourcePerSecond = 2)` | Under load once more than `handshakesPerSecond` handshake messages arrive within a second. Under load, a message without mac2 gets a cookie, and only those with a valid mac2 are handled, up to `perSourcePerSecond` a second per source address |

About 150 B of RAM (the cookie secret and a table of 8 sources). The secret is renewed every 2 minutes.

## tinywg::Device

The engine, with no lwIP or Arduino dependency. Not thread-safe: the caller serialises every call. `nowMs` is a 32-bit millisecond count like `millis()`; wrapping is handled.

| Method | Description |
|---|---|
| `bool begin(const uint8_t privateKey[32], Transport *transport)` | Starts with the private key and the output. false with no random source |
| `void end()` | Wipes peers and keys |
| `void setRandom(RandomFn fn)` / `void setWallClock(WallClockFn fn)` | As in `TinyWGBase` |
| `int addPeer(...)` / `bool removePeer(int)` / `bool setEndpoint(int, const Endpoint &)` / `bool setPersistentKeepalive(int, uint16_t)` / `bool setMtu(uint16_t)` / `uint16_t mtu() const` / `void setTimestampReset(uint16_t)` / `bool setStagingBuffer(uint8_t *, size_t)` / `void setHandshakeGuard(HandshakeGuard *)` (what `setCookieGuard()` calls) | As in `TinyWGBase` |
| `bool addAllowedIp(int peer, const uint8_t addr[4], uint8_t cidr)` | Address as bytes |
| `int findPeer(const uint8_t publicKey[32]) const` | The peer with this public key, or -1 |
| `int routePeer(const uint8_t addr[4]) const` | The peer a packet to this address goes to (longest AllowedIPs match), or -1 |
| `bool peerStatus(int peer, PeerStatus *out) const` | Peer state |
| `const Stats &stats() const` | Counters |
| `const uint8_t *publicKey() const` | Public key (32 bytes) |
| `void receiveOuter(uint8_t *data, size_t len, const Endpoint &from, uint32_t nowMs)` | One outer UDP datagram. `data` is decrypted in place |
| `uint8_t *innerBuffer()` | Room for one inner packet (`mtu()` bytes). A packet written here and passed to `sendInner()` is encrypted in place, with no copy. Overwritten by every call into the engine |
| `SendResult sendInner(const uint8_t *packet, size_t len, uint32_t nowMs)` | Sends one inner IPv4 packet |
| `bool connect(int peer, uint32_t nowMs)` | Starts a handshake now |
| `void tick(uint32_t nowMs)` | Runs the timers; call every 100 ms or so |
| `static constexpr uint16_t kMinMtu = 576` | Lowest MTU |

### Transport

Where `Device`'s output goes. Implement it and pass it to `begin()`.

```cpp
class Transport {
public:
    virtual void sendOuter(const Endpoint &to, const uint8_t *data, size_t len) = 0;
    virtual void deliverInner(int peer, uint8_t *packet, size_t len) = 0;
};
```

- `sendOuter`: send one outer UDP datagram to `to`
- `deliverInner`: an authenticated inner IPv4 packet. `packet` points into the buffer given to `receiveOuter()` and may be modified

## Types

### SendResult

| Value | Meaning |
|---|---|
| `Sent` | Encrypted and sent |
| `Queued` | Held until the handshake completes, then sent (where it waits: `setStagingBuffer()`) |
| `Dropped` | No session, and nowhere to hold it |
| `NoRoute` | No peer's AllowedIPs covers the destination |
| `NoEndpoint` | Nowhere to send (no Endpoint set, nothing heard from the peer) |
| `Invalid` | Not IPv4, or larger than the MTU |

### PeerStatus

| Field | Type | Meaning |
|---|---|---|
| `sessionUp` | `bool` | A session that can send right now |
| `handshakeInFlight` | `bool` | An initiation waiting for its response |
| `hasHandshake` | `bool` | A handshake has completed at least once |
| `lastHandshakeMs` | `uint32_t` | Time (ms) of the latest completed handshake |
| `endpoint` | `Endpoint` | Where packets go now |
| `rxBytes` / `txBytes` | `uint64_t` | Outer UDP bytes (WireGuard header and tag included) |

### Stats

All `uint32_t`. What each means and its usual cause: [advanced guide §10](guide-advanced.md#10-monitoring-and-diagnosis).

`rxShort`, `rxBadMac1`, `rxHandshakeFail`, `rxUnknownPeer`, `rxStaleTimestamp`, `rxFlood`, `rxNoSession`, `rxDecryptFail`, `rxReplay`, `rxNotAllowed`, `rxNotIpv4`, `handshakesInitiated`, `handshakesCompleted`, `handshakesResponded`, `cookiesSent`, `rxUnderLoad`

### Endpoint

```cpp
struct Endpoint {
    uint8_t addr[4];   // IPv4 address
    uint16_t port;     // 0 = not set
};
```

### Function types

```cpp
// Fill out with len cryptographically secure random bytes; false when unavailable
typedef bool (*RandomFn)(uint8_t *out, size_t len);
// The current time as Unix seconds and nanoseconds; must not go back across reboots
typedef void (*WallClockFn)(uint64_t *unixSeconds, uint32_t *nanoseconds);
```

## Key functions

In namespace `tinywg`.

| Function | Description |
|---|---|
| `bool generatePrivateKey(uint8_t privateKey[32], RandomFn random = nullptr)` | Makes a private key; without `random`, the default source. false with no source |
| `void derivePublicKey(uint8_t publicKey[32], const uint8_t privateKey[32])` | Computes the public key |
| `bool keyFromBase64(uint8_t key[32], const char *base64)` | Base64 to bytes; false for a malformed string |
| `void keyToBase64(char out[45], const uint8_t key[32])` | Bytes to Base64 (44 characters + NUL) |

## Build-time settings

Override with `-D`. Values and their effect on RAM: [advanced guide §8](guide-advanced.md#8-mtu-and-memory).

`TINYWG_MAX_PEERS` (4), `TINYWG_MAX_ALLOWED_IPS` (4), `TINYWG_MTU` (1420), `TINYWG_REPLAY_WINDOW` (128)

The version is in `TINYWG_VERSION_MAJOR` / `TINYWG_VERSION_MINOR` / `TINYWG_VERSION_PATCH` / `TINYWG_VERSION_STR` (`tinywg_version.h`).
