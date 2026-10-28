# Tests

[日本語](README.ja.md)

As in PCMFlowUDP, the tests run on [pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) with the Arduino CLI. The default profile is `lang-ship:host` (sketches that run on the PC).

```sh
cd tests
uv run pytest smoke crypto_vectors core_pair time_server bench_crypto
unshare -rn uv run pytest kernel_interop      # interop with the Linux kernel's WireGuard
```

## The tests

| Directory | What it checks | Profiles |
|---|---|---|
| `smoke/` | The build and the harness | host / esp32 / ch32v307 / picow |
| `crypto_vectors/` | BLAKE2s, MAC, KDF, X25519 and ChaCha20-Poly1305 output against values computed independently with Python's `hashlib` / `cryptography`; AEAD tamper detection, refusal of low-order points, Base64 | as above |
| `core_pair/` | Two engines face to face in one process: handshake, data both ways, refusal of replays and tampering, AllowedIPs, reordering, keepalives, rekey and key zeroing, retransmission, a wrong PSK, receiving a cookie reply and mac2, `CookieGuard` (cookies under load, mac2 tied to the source), a staged packet and `setStagingBuffer()`, a rebooted peer whose clock went back and `setTimestampReset()` | as above |
| `time_server/` | `TinyWGTimeServer` replies (mode, version, stratum, origin time, the time served), requests it must ignore, and a real NTP client when chrony is installed | host only |
| `kernel_interop/` | Interop with the Linux kernel's WireGuard: the kernel starts (TinyWG responds), TinyWG starts (initiator), ping, exactly MTU 1420, PSK, a wrong PSK, a burst of 300 packets, the kernel taking a `CookieGuard` cookie and resending, setting up from wg-quick text (`beginConfig()`) | host only |
| `bench_crypto/` | Phase 0 measurements: X25519, each handshake step, AEAD / BLAKE2s speed, the engine's RAM | host / esp32 / esp32s3 / ch32v307 / picow |
| `pypeer_selftest/` | The Python WireGuard peer used on the PC side of the board tests (`helpers/wg_peer.py`), checked against the kernel's WireGuard (initiator / responder, with a PSK) | PC only (no sketch) |
| `board_responder/` | A board runs TinyWG (lwIP's wg0) on Wi-Fi (ESP32) or Ethernet (CH32V307), reached by the Python peer on the PC: ping (answered by lwIP), UDP echo up to 1392 B, 100 round trips, a new handshake from the board, PSK, refusal of an unknown key, a run-time MTU change (at 1000 the replies come in fragments), and on ESP32 NAPT (ping and DNS to the LAN router through the tunnel). Three outer modes | esp32 / esp32s3 (Wi-Fi in `.env`) / ch32v307 (LAN cable) |
| `board_responder/test_board_longrun.py` | A long run of about 14 minutes: rekey from the board (120 s), persistent keepalive, key zeroing without traffic (540 s) and recovery. Runs only with `TEST_LONG=1` | esp32 / esp32s3 / ch32v307 |
| `board_responder/test_board_config.py` | wg0 brought up from wg-quick text with `TinyWG::beginConfig()`, then echo from the PC | esp32 / esp32s3 / ch32v307 |
| `board_responder/test_board_wifi.py` | Wi-Fi leaving and rejoining (`WIFIDROP`): back on the same keys after a short drop, a handshake started off the AP completing by retransmission after rejoining, the board following a port change on the PC side (a NAT rebinding) | esp32 / esp32s3 |
| `board_pair/` | Two boards: the primary opens a SoftAP and is the WireGuard server, `peer_client/` joins and is the client. TCP echo (32 KB) through the tunnel, in four outer modes of the two sides | every esp32 / esp32s3 pair (no external AP) |

## How kernel_interop works

Creating the kernel's wg0 needs CAP_NET_ADMIN. `unshare -rn` makes a new user and network namespace and runs all of pytest inside it; no root is needed and the real network is not touched. The namespace has only `lo`, which `conftest.py` brings up at start. Without the privilege the test is skipped.

- Kernel side: wg0 = 10.9.0.1/24 on 127.0.0.1:51820 (`helpers/wg_kernel.py`, set over netlink with pyroute2)
- DUT: 10.9.0.2 on 127.0.0.1:51821, answering ICMP echo and UDP echo (port 7) inside the tunnel

The host DUT accepts one serial connection only. So a single test function reconfigures it with fresh keys for each scenario (the `CFG` command) and reports each one as `SCENARIO PASS/FAIL` and as a junit property.

## On the boards

Copy `.env.example` to `.env` and fill in the serial ports and Wi-Fi. Wi-Fi reaches the sketches as `WIFI_SSID` / `WIFI_PASSWORD` through `build_config.toml`.

```sh
uv run --env-file .env pytest crypto_vectors core_pair bench_crypto --profile esp32 -s
uv run --env-file .env pytest board_responder --profile esp32 -s
uv run --env-file .env pytest board_responder --profile ch32v307 -s
uv run --env-file .env pytest board_pair --profiles=all --peer-profiles=all -s
```

`board_responder` and `board_pair` run with the outer UDP on lwIP and on an Arduino `UDP` (`setOuterUdp()`): three ways for `board_responder` (lwIP; replaced and driven by the library; replaced and driven by `update()`), and 2 × 2 = four for `board_pair`, server by client. Each parameter starts by restarting the board with the `RESTART` command.

`board_pair` runs over Wi-Fi, so any pair of boards works, and it runs over the full product `--profiles=all --peer-profiles=all`. A pair that would need the same board on both sides is skipped by the plugin. As the boards swap roles, the client's port is given per profile as `TEST_SERIAL_PORT_PEER_CLIENT_<PROFILE>` (see `.env.example`).

In `board_responder` the PC starts the handshake: the answer comes back through NAT even when the PC sits behind one (WSL2 and the like). Once the board knows the endpoint, a handshake from the board is tried too.

## Full run before a release

Before a release, rebuild everything (`--clean`) and run it all. Every connected board is used; tests for boards without a port are skipped.

```sh
cd tests
# 1. Interop with the Linux kernel's WireGuard (inside a network namespace)
unshare -rn uv run pytest kernel_interop pypeer_selftest --clean

# 2. The host and every connected board: every profile of each sketch, and every board pair for board_pair
TEST_LONG=1 uv run --env-file .env pytest --profiles=all --peer-profiles=all --clean
```

- In 2, `kernel_interop` and `pypeer_selftest` skip for lack of privilege; that is why 1 is separate. The board tests use the PC's real network and cannot run inside the namespace
- 2 includes the long run `board_responder/test_board_longrun.py` (only with `TEST_LONG=1`; about 14 minutes per board) and the Wi-Fi drop tests
- Do not take a skip for a pass. In the final summary, check that the only skip reasons are "port not set" and "same board on both sides" (`-rs` lists them)
- Boards shared with others (the CH32V307 here, for one) must be reserved by the agreed procedure first

## Measuring on a board

```sh
uv run pytest bench_crypto -s --profile esp32 --port /dev/ttyUSB0
```

Values are printed as `BENCH <name> <value> <unit>` and kept as junit properties too.
