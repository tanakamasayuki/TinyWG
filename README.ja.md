# TinyWG

[English](README.md)

Arduino 向けの、コアに依存しない WireGuard 実装です。**イニシエータ(クライアント)とレスポンダ(サーバー)の両方**に対応し、複数のピアを扱えます。

- プロトコルエンジン(`tinywg::Device`)は lwIP にも Arduino API にも依存しません
- `TinyWG` は lwIP のネットワークインターフェース(wg0)として動きます。`WiFiClient` / `NetworkClient`、`WiFiServer`、MQTT ブローカーなど、lwIP 上の既存のコードがそのままトンネルを通ります
- `TinyWGUdp` は任意の Arduino `UDP` の上で動き、内側の IPv4 パケットをスケッチが直接扱います
- Linux カーネルの WireGuard と相互接続できることを自動テストで確認しています

TLS が重い、または載らないマイコンで、暗号化された経路を確保するためのライブラリです。TLS を載せるより Flash も RAM も小さく済みます。

## 対応状況

| コア | `TinyWG` (lwIP) | `TinyWGUdp` | 乱数源 | 確認状況 |
|---|---|---|---|---|
| ESP32 (arduino-esp32 3.3) | ○(NAPT 可) | ○ | `esp_fill_random` | ビルドのみ |
| RP2040 / RP2350 (arduino-pico 6.3、Pico W / Pico 2 W) | ○(FreeRTOS モードは未対応) | ○ | `get_rand_64` | ビルドのみ |
| CH32V (ArduinoCore-CH32RV 0.4、Network ライブラリ) | ○ | ○ | TRNG(V307 など) | ビルドのみ |
| lang-ship:host(PC 上のテスト用) | — | ○ | `/dev/urandom` | Linux カーネル WireGuard と相互接続テスト済み |

実機での確認と、実機上での速度計測はまだです(`tests/bench_crypto` で計測できます)。

### フットプリント(同じ機能のスケッチに TinyWG を足した分)

| | Flash | RAM(静的) |
|---|---|---|
| ESP32 | +25.6 KB | +8.2 KB |
| Pico W | +22.9 KB | +7.9 KB |
| CH32V307 | +22.2 KB | +7.9 KB |

既定の設定(ピア 4、MTU 1420)での値です。動的確保はしません。

## 使い方(クライアント)

```cpp
#include <WiFi.h>
#include <TinyWG.h>

TinyWG wg;

void setup()
{
    // ... Wi-Fi に接続し、NTP で時刻を合わせる(下の「時刻」参照)
    wg.begin(IPAddress(10, 9, 0, 2), IPAddress(255, 255, 255, 0), "<このデバイスの秘密鍵>", 0);
    int server = wg.addPeer("<サーバーの公開鍵>");
    wg.addAllowedIp(server, IPAddress(10, 9, 0, 0), 24);
    wg.setEndpoint(server, IPAddress(203, 0, 113, 1), 51820);
    wg.setPersistentKeepalive(server, 25);
}
```

`begin()` の後は、10.9.0.0/24 宛ての通信が lwIP によって wg0 に流れます。`loop()` から呼ぶものはありません。

## 使い方(サーバー)

```cpp
wg.begin(IPAddress(10, 9, 0, 1), IPAddress(255, 255, 255, 0), "<サーバーの秘密鍵>", 51820);
int a = wg.addPeer("<ピア A の公開鍵>");
wg.addAllowedIp(a, IPAddress(10, 9, 0, 2), 32);
int b = wg.addPeer("<ピア B の公開鍵>");
wg.addAllowedIp(b, IPAddress(10, 9, 0, 3), 32);
```

相手から接続してくるピアには Endpoint は不要です。最後に認証できたパケットの送信元に自動で追従します(ローミング)。ESP32 では `wg.enableNapt()` で、ピアからの LAN / インターネット宛ての通信を NAPT で中継できます。

鍵は `wg genkey` / `wg pubkey` で作れます。

## 時刻

イニシエータが送るハンドシェイクには時刻(TAI64N)が入り、受け手は前回より新しいものしか受け付けません。RTC のないボードで再起動後に時刻が戻ると、相手側(Linux など)に拒否されます。**クライアントとして使うときは、`begin()` の前に NTP で時刻を合わせてください。** レスポンダとしてだけ使う場合は時刻は不要です。`setWallClock()` で時刻の取得方法を差し替えられます。

## 設定(ビルド時)

`-D` で上書きできます(`src/tinywg/wg_config.h`)。

| マクロ | 既定値 | |
|---|---|---|
| `TINYWG_MAX_PEERS` | 4 | ピア数 |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | ピアごとの AllowedIPs の数 |
| `TINYWG_MTU` | 1420 | トンネル内の MTU |
| `TINYWG_REPLAY_WINDOW` | 128 | リプレイ防止の窓(ビット) |
| `TINYWG_STAGED_PACKET` | 1 | ハンドシェイク中の 1 パケットを保持する |

## 制限

- IPv4 のみ(内側・外側とも)
- Cookie Reply は受信のみ(高負荷時にこちらから送る機能はなし)
- arduino-pico の FreeRTOS モードは未対応
- wg0 を既定経路にした場合、外側の UDP がトンネルに入るものは送信しません(ループ防止)

## ライセンス

BSD 3-Clause。同梱している [Monocypher](src/third_party/monocypher/)(X25519、ChaCha20-Poly1305)は BSD-2-Clause / CC0 のデュアルライセンスです。BLAKE2s は RFC 7693 から、プロトコル層は WireGuard のホワイトペーパーから実装しており、GPL のコードは使っていません。

"WireGuard" は Jason A. Donenfeld の登録商標です。本ライブラリは WireGuard プロジェクトとは無関係で、プロトコル名を記述的に使用しています。
