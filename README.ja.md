# TinyWG

[English](README.md)

Arduino 向けの、コアに依存しない WireGuard 実装です。**イニシエータ(クライアント)とレスポンダ(サーバー)の両方**に対応し、複数のピアを扱えます。

- プロトコルエンジン(`tinywg::Device`)は lwIP にも Arduino API にも依存しません
- `TinyWG` は lwIP のネットワークインターフェース(wg0)として動きます。`WiFiClient` / `NetworkClient`、`WiFiServer`、MQTT ブローカーなど、lwIP 上の既存のコードがそのままトンネルを通ります
- `TinyWGUdp` は任意の Arduino `UDP` の上で動き、内側の IPv4 パケットをスケッチが直接扱います
- Linux カーネルの WireGuard と相互接続できることを自動テストで確認しています

使いみちの例:

- **ふつうに WireGuard として**: 既存の WireGuard のネットワーク(Linux のサーバー、PC やスマホの公式アプリ)にマイコンを参加させる。マイコンを WireGuard のサーバーにして、外から入ることもできる
- **軽い暗号化として選ぶ**: TLS が載るマイコンでも、鍵の交換が 1 往復で済み、Flash と RAM も小さい WireGuard を選ぶ
- **TLS が重い、または載らないマイコンで**: 暗号化された経路をつくる

## 対応状況

| コア | `TinyWG` (lwIP) | `TinyWGUdp` | 乱数源 | 確認状況 |
|---|---|---|---|---|
| ESP32 / ESP32-S3 (arduino-esp32 3.3) | ○(NAPT 可) | ○ | `esp_fill_random` | 実機で確認(PC との接続、ボード 2 台での TCP、NAPT、長時間試験、Wi-Fi の切断からの復帰、計測) |
| RP2040 / RP2350 (arduino-pico 6.3、Pico W / Pico 2 W) | ○(FreeRTOS モードは未対応) | ○ | `get_rand_64` | ビルドのみ |
| CH32V (ArduinoCore-CH32RV 0.4、Network ライブラリ) | ○ | ○ | TRNG(V307 など) | V307 の実機で確認(Ethernet で PC との接続、長時間試験、計測) |
| lang-ship:host(PC 上のテスト用) | — | ○ | `/dev/urandom` | Linux カーネル WireGuard と相互接続テスト済み |

ESP32 では X25519 が 1 回 17.6 ms、レスポンダの応答処理が 72.7 ms(S3 は 12.5 ms / 52.0 ms)でした。詳細は [SPEC.ja.md](SPEC.ja.md) §5.2。

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

`begin()` の後は、10.9.0.0/24 宛ての通信が lwIP によって wg0 に流れます。`loop()` から呼ぶものはありません。persistent keepalive を設定すると、その時点で keepalive を 1 つ送る(セッションがなければハンドシェイクを始める)ので、送るものがなくてもすぐにつながります。

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

## MTU

トンネル内の MTU は既定で 1420 です。外側の経路が 1500 より小さいとき(PPPoE、LTE、別のトンネルの中など)は、実行時に下げられます。`begin()` の前でも後でも構いません。外側の経路には「MTU + 60 バイト」が通る必要があります。

```cpp
wg.setMtu(1280);   // 576 〜 TINYWG_MTU。lwIP の wg0 の MTU も変わり、TCP のセグメントもこれに合わせて小さくなる
```

`TINYWG_MTU` はバッファの大きさを決める上限です。大きな MTU を使わないなら、これも下げると RAM が減ります。

## 時刻

イニシエータが送るハンドシェイクには時刻(TAI64N)が入り、受け手は前回より新しいものしか受け付けません。RTC のないボードで再起動後に時刻が戻ると、相手側(Linux など)に拒否されます。**クライアントとして使うときは、`begin()` の前に NTP で時刻を合わせてください。** レスポンダとしてだけ使う場合は時刻は不要です。`setWallClock()` で時刻の取得方法を差し替えられます。

## 設定(ビルド時)

`-D` で上書きできます(`src/tinywg/wg_config.h`)。

| マクロ | 既定値 | |
|---|---|---|
| `TINYWG_MAX_PEERS` | 4 | ピア数 |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | ピアごとの AllowedIPs の数 |
| `TINYWG_MTU` | 1420 | トンネル内の MTU の上限(バッファの大きさ)。使う MTU は `setMtu()` で実行時に下げられる |
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
