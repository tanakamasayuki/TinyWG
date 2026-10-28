# TinyWG specification (draft)

[日本語](SPEC.ja.md)

> Status: phases 0 to 3 implemented (2026-10-27). ESP32, ESP32-S3 and CH32V307 verified on hardware; Pico W / Pico 2 W build only. Values not decided yet, such as the smallest target, are marked `TBD`.
>
> For how to use the library see the [beginner's guide](docs/guide-beginner.md) and the [advanced guide](docs/guide-advanced.md). This document records the design and the measurements.

## 1. Purpose and scope

**TinyWG** is a portable WireGuard implementation for Arduino.

- Unlike the existing libraries written for one core (ESP32, Pico), it is a **core-independent WireGuard engine plus a thin porting layer**
- It is not only an initiator (client) but also a **responder (server)**, with several peers
- Three uses rank equally: ordinary WireGuard, joining an existing WireGuard network (or serving one); WireGuard chosen as the lighter encryption on an MCU that could run TLS; an encrypted path on MCUs where TLS is too heavy or does not fit

### 1.1 Use cases

| # | Set-up | Role of TinyWG |
|---|---|---|
| UC1 | Small MCU (CH32V and others) → WireGuard server on the Internet (Linux and others) | initiator, one peer |
| UC2 | An IoT gateway (ESP32 and others) on the LAN as WireGuard server and MQTT broker; small MCUs connect over WireGuard | gateway: responder, several peers / devices: initiators |
| UC3 | A PC or phone connects straight to a WireGuard server on the MCU for maintenance | responder |

### 1.2 Target platforms

Every target is assumed to **have lwIP**.

| Core | NIC | lwIP | Notes |
|---|---|---|---|
| ESP32 (arduino-esp32 3.3) | Wi-Fi / Ethernet | esp-lwip: with an OS, tcpip thread, `LWIP_TCPIP_CORE_LOCKING=y`, thread-safety checks | `IP_FORWARD` / `IP_NAPT` enabled |
| RP2040 / RP2350 (arduino-pico 6.3) | CYW43 Wi-Fi / Ethernet | upstream lwIP, `NO_SYS=1`. The lwIP API is wrapped (`--wrap`) in a recursive mutex (`LWIPMutex`). In FreeRTOS mode requests go to a dedicated lwIP task | `IP_FORWARD` disabled, no NAPT |
| CH32V (ArduinoCore-CH32RV 0.4, Network library) | built-in Ethernet / USB NIC (NCM) | lwIP 2.2.1, `NO_SYS=1`, raw API, run after `loop()` and inside `delay()`; lwIP ships with the Network library | `IP_FORWARD` disabled. TRNG only on V303/305/307/317, V407/467, H41x (not V208) |
| host (lang-ship:host) | loopback | none (for tests) | `TinyWGUdp` only: checks of the engine and crypto, interop tests with the Linux kernel WireGuard |

The smallest target chip is `TBD`.

## 2. Out of scope

- An IP stack of our own for cores without lwIP (maybe later; not in the initial scope)
- IPv6 inside the tunnel `TBD` (depends on lwIP)
- NAT traversal (STUN and the like), DNS
- VPN protocols other than WireGuard

## 3. Architecture

```
App (MQTT broker/client, CoAP, any socket)
   │  plain lwIP API
[lwIP]
   │  netif (wg0)  ← provided by TinyWG
[WG Netif Adapter]    lwIP netif input/output, routing, NAPT
[WG Core]             handshake / sessions / timers / peer table / AllowedIPs   ← core-independent
[Crypto]              X25519 / ChaCha20-Poly1305 / XChaCha20-Poly1305 (cookie) / BLAKE2s
[Outer Transport]     outer UDP: lwIP udp_pcb directly (TinyWG) / Arduino UDP& (TinyWGUdp)
[Port Layer]          random / time / locking / timer drive   ← per core
```

### 3.1 Responsibilities

- **WG Core**: plain C/C++ with no lwIP or Arduino dependency. In: outer UDP datagrams and inner IP packets; out: the same. Unit-testable on the host
- **Port Layer**: keeps the per-core differences in one place
  - cryptographic random numbers (ESP32: `esp_fill_random`, RP2040 / RP2350: `get_rand_64`, CH32V: TRNG; series without a TRNG and other cores: supplied by the user with `setRandom()`)
  - time (a monotonic clock like `millis()` plus a wall clock for TAI64N, §4.4)
  - locking (esp-lwip: against the tcpip thread; arduino-pico: `LWIPMutex`; CH32V: none; §3.2)
- **Outer Transport**: both are implemented
  - `TinyWG` (lwIP adapter): uses lwIP's `udp_pcb` directly. A received pbuf that is contiguous is decrypted in place
  - `TinyWGUdp`: an Arduino `UDP&`. Inner packets are raw IPv4 handed to a callback (no IP stack). For host tests and cores without lwIP

### 3.2 Execution context and locking (implemented)

In `TinyWG`, the outer UDP receive callback, the netif output and a 100 ms lwIP timer (`sys_timeout`) all run in lwIP's context. Nothing is called from `loop()`. The public API called by the sketch (adding peers and so on) serialises with lwIP in a per-core way.

| Core | Locking |
|---|---|
| ESP32 | `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()`. Calling the public API from an lwIP callback deadlocks |
| arduino-pico | `LWIPMutex` (the same recursive lock as lwIP's wrappers). FreeRTOS mode is a compile error (not supported) |
| CH32V | none (lwIP runs only after `loop()` and inside `delay()`) |

When the endpoint itself would be routed into wg0 (the outer UDP entering the tunnel), nothing is sent, to avoid a loop (checked with `ip4_route()`).

## 4. Functional requirements

### 4.1 Protocol (required)

- Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s handshake (Initiation / Response)
- Transport Data / Keepalive / PersistentKeepalive (one is sent as soon as it is set, as Linux does)
- PresharedKey (optional)
- Rekey timers (REKEY_AFTER_TIME / REJECT_AFTER_TIME / REKEY_AFTER_MESSAGES / REKEY_TIMEOUT and others)
- current / previous / next session rotation
- Sliding replay window (size set at build time with `TINYWG_REPLAY_WINDOW`, 128 bits by default)
- Cookie Reply: received and processed. Sending one (under load) is not implemented

### 4.2 Responder / gateway

- Listening on a fixed ListenPort, several peers (limit set at build time with `TINYWG_MAX_PEERS`, 4 by default)
- Cryptokey routing by AllowedIPs (peer chosen by destination on send, source checked on receive)
- Endpoint roaming (updated to the source of the latest authenticated packet)
- Ordinary lwIP sockets (an MQTT broker and so on) can listen on the wg0 netif
- NAPT / forwarding: on ESP32, with `IP_NAPT`, devices on the WireGuard side reach the LAN / Internet. Other cores: routing only
- Peer-to-peer traffic through the gateway `TBD`

### 4.3 Configuration

- Keys and peers set through the API
- Parsing `wg-quick` text (`[Interface]` / `[Peer]`) `TBD` (depends on the footprint)
- Keys accepted in Base64

### 4.4 Time (TAI64N)

- The initiator's TAI64N timestamps must increase. An MCU without an RTC is refused by the peer (Linux and others) after a reboot, as its timestamp looks old
- The default is `gettimeofday()` (a wall clock set over NTP / SNTP). `setWallClock()` replaces it with a user function (an RTC, a value built from a boot counter in non-volatile storage, and so on; example in the advanced guide §6)
- Within one boot the values sent keep increasing even if the wall clock steps back
- `setTimestampReset(seconds)` on the responder: a peer that has sent neither a response nor data for that long has its last timestamp forgotten, and an older one is accepted. For clients with no clock on closed networks. Default 0 (never forget, as the standard). While a peer is silent, replaying a recorded initiation can then disrupt its traffic (no key leak, impersonation or injection)
- A responder only compares received values and needs no wall clock

## 5. Non-functional requirements

| Item | Policy |
|---|---|
| Memory allocation | No dynamic allocation, or only at initialisation. Peer table and sessions are static |
| Execution model | The engine assumes no threads. `TinyWG` runs in lwIP's context (the receive callback and a 100 ms timer) and its public API locks against lwIP (§3.2). `TinyWGUdp` is polled: `update()` from `loop()` |
| Flash / RAM | Measured (§5.1). Targets to be set from the measurements |
| Handshake time | Measured (§5.2) with `tests/bench_crypto` on each core |
| Throughput | Measured (§5.2) |
| Inner MTU | Changeable at run time with `setMtu()` from 576 up to the limit (1420 by default). The limit `TINYWG_MTU` sizes the buffers and can be lowered at build time to save RAM |
| Crypto backend | Portable C (Monocypher, BLAKE2s). Hardware acceleration (ESP32 and others) `TBD` |

### 5.1 Footprint (2026-10-27, build sizes)

What TinyWG adds to a sketch with the same function (network, UDP send, clock). Default settings (4 peers, MTU 1420, 128-bit window, staged packet on).

| Target | Flash | RAM (static) | Notes |
|---|---|---|---|
| ESP32 (Esp32Client) | +25.6 KB | +8.2 KB | |
| Pico W (PicoWClient) | +22.9 KB | +7.9 KB | |
| CH32V307 (Ch32EthernetClient) | +22.2 KB | +7.9 KB | 79.3 KB flash / 45.5 KB RAM in all (lwIP pools included) |

RAM breakdown (`sizeof` on a 64-bit host): `Device` 6.3 KB (792 B per peer × 4, send buffer 1.47 KB, staged packet 1.42 KB), plus the lwIP adapter's scratch buffer of 1.47 KB. Slightly smaller on 32-bit MCUs.

Candidates for savings (decided by measurement): disabling the staged packet, fewer peers or a smaller MTU, sharing the lwIP adapter's scratch buffer with `Device`'s send buffer, passing received pbufs to `ip_input` without a copy.

### 5.2 Measurements on hardware (2026-10-27, `tests/bench_crypto`, Arduino's default optimisation)

| | ESP32 (D0WD-V3, 240 MHz) | ESP32-S3 (240 MHz) | CH32V307 (96 MHz, core default) |
|---|---|---|---|
| One X25519 | 17.6 ms | 12.5 ms | 24.3 ms |
| Handshake: create initiation (initiator) | 36.3 ms | 26.1 ms | 51.2 ms |
| Handshake: receive and respond (responder) | 72.7 ms | 52.0 ms | 103.2 ms |
| Handshake: receive response, complete (initiator) | 36.5 ms | 26.1 ms | 52.1 ms |
| ChaCha20-Poly1305 (1420 B) | 2.6 MB/s (0.53 ms/packet) | 3.5 MB/s (0.40 ms/packet) | 1.6 MB/s (0.86 ms/packet) |
| BLAKE2s | 4.0 MB/s | 5.3 MB/s | 1.3 MB/s |
| `sizeof(Device)` | 6,280 B | 6,280 B | 6,280 B |

Two boards (`tests/board_pair`, ESP32 as SoftAP and server, S3 as client): handshake 171 ms (Wi-Fi round trips included), a 32 KB TCP echo through the tunnel at about 376 KiB/s both ways, no errors.

PC → board (`tests/board_responder`, through a home AP, started by the Python peer on the PC): handshake round trip ESP32 94 ms / S3 100 ms, ping through the tunnel about 3 ms, 100 round trips of a 200 B UDP echo (both ways about 61 KiB/s on ESP32, 69 KiB/s on S3; one at a time, so bound by Wi-Fi latency), a new handshake from the board, PSK, and refusal of an unknown key all pass. The CH32V307 passes the same test over Ethernet (handshake round trip 106 ms, ping about 1 ms, 100 UDP echoes at about 158 KiB/s both ways). `crypto_vectors` / `core_pair` / `smoke` also pass on the boards.

Long run (`tests/board_responder/test_board_longrun.py`, ESP32, about 14 minutes): the board rekeys at exactly 120 s (no echo lost meanwhile), persistent keepalive every 15 s gives 4 in 65 s, with no traffic the keys are zeroed 547 s after the last handshake (checked every 20 s, so consistent with 540 s), and a handshake from the PC brings it back. The CH32V307 (Ethernet; lwIP's timers run after `loop()` and inside `delay()`) gives the same: rekey at 120 s (nothing lost), 5 keepalives in 65 s (including the one sent when it was set), keys zeroed after 547 s, then back again.

Wi-Fi outages (`tests/board_responder/test_board_wifi.py`, ESP32 / S3): after 5 s off the AP the session carries on with the same keys (about 3 s to join again, then a keepalive tells the PC where the board is). A handshake the board starts while off the AP is retransmitted every 5 s without getting out, and completes 2.5 to 3 s after joining again. When the PC's port changes (a NAT rebinding), the board follows.

The Pico W has not been measured on hardware yet.

## 6. Testing

Follows the test set-up of PCMFlowUDP (pytest-embedded + Arduino CLI, the `TEST done N/M` protocol).

- **host (`lang-ship:host`)**: crypto test vectors (RFC 7748 / 8439 / 7693), handshakes and data between two WG Cores
- **Python peer**: a test WireGuard peer built on `hashlib.blake2s` + `cryptography`
- **Interop**: the Linux kernel WireGuard, checked by automated tests (`tests/kernel_interop`). wireguard-go and the official mobile apps not checked yet
- **Footprint**: build size, RAM and handshake time recorded per target. The release workflow has a call to `tools/release_hooks/pre_version_commit.py`, but the script does not exist yet (recorded by hand in §5 for now)

## 7. Plan

Progress (2026-10-27):

- Phase 0: done. Footprint, and speed on ESP32 / ESP32-S3 / CH32V307 hardware (not on Pico W)
- Phase 1: done. Pair tests on the host and interop with the Linux kernel WireGuard (as initiator and as responder) pass
- Phase 2: verified on ESP32 / ESP32-S3 hardware (unit tests, measurements, TCP between two boards, PC-to-board tests `board_responder`)
- Phase 3: the CH32V307 passes the unit tests, the measurements, `board_responder` over Ethernet and the long run on hardware. arduino-pico (RP2040 / RP2350) build only
- Phase 4: ESP32 NAPT (`enableNapt()`) verified on hardware (`board_responder`: ping and DNS to the LAN router through the tunnel). The MQTT broker example not started

1. **Phase 0 — measure**: build the crypto primitives alone for each target; measure flash/RAM, X25519 time and ChaChaPoly throughput. Compare the outer UDP options (`udp_pcb` directly / Arduino `UDP&`)
2. **Phase 1 — WG Core**: handshake, sessions and timers on the host, interop with the Python peer
3. **Phase 2 — lwIP netif adapter**: wg0 netif on ESP32, interop with Linux `wg` (initiator and responder)
4. **Phase 3 — ports**: arduino-pico, CH32RV
5. **Phase 4 — gateway**: several peers, ESP32 NAPT, an MQTT broker example
6. Settle the smallest target and the defaults from the results of phases 0 to 4

## 8. Licence and trademark

- TinyWG itself is **BSD 3-Clause**
- Bundled third-party code (crypto and so on) keeps its licence and copyright notices, listed in the README. Only permissive licences compatible with BSD-3 (CC0 / PD / BSD / MIT)
- The protocol layer is written from public specifications such as the whitepaper. **No GPL code (the Linux kernel WireGuard and others) is consulted or reused**
- "WireGuard" is a registered trademark of Jason A. Donenfeld. This library is not affiliated with the WireGuard project and uses the protocol name descriptively
