# Esp32W5500Client

[English](README.md) | [上級者ガイド §2](../../docs/guide-advanced.ja.md#外側の-udp-を差し替える)

W5500 を Arduino の `Ethernet` ライブラリで動かす ESP32 から、WireGuard サーバーに接続します。5 秒ごとに、トンネル越しに UDP の `hello from TinyWG over W5500` を `10.9.0.1:9000` に送ります。

`Ethernet` ライブラリでは、TCP/IP が W5500 の中で動きます。ESP32 の lwIP の外なので、`wg0` は lwIP に置いたまま、`setOuterUdp()` で暗号化した外側の UDP だけを W5500(`EthernetUDP`)から送ります。トンネルの中は lwIP なので、`NetworkUDP` や `NetworkClient` がそのまま使えます。

W5500 を ESP32 の `ETH`(lwIP に載せる使い方)で動かす場合は、このサンプルは要りません。`Esp32Client` と同じく、ふつうの `TinyWG` で動きます。

## 必要なもの

- ESP32(arduino-esp32 3.3 以降)と W5500 のモジュール
- Arduino の `Ethernet` ライブラリ 2.0.2(`sketch.yaml` のプロファイルで入ります。Arduino IDE ではライブラリマネージャから)
- WireGuard のサーバー(Linux の `wg-quick`、公式アプリなど)

## 配線

| W5500 | ESP32 |
|---|---|
| SCK | GPIO 18 |
| MISO | GPIO 19 |
| MOSI | GPIO 23 |
| CS | GPIO 5(`kCsPin`) |
| 3.3V / GND | 3.3V / GND |

## 設定する項目

| 定数 | 内容 |
|---|---|
| `kCsPin` | W5500 のチップセレクトのピン |
| `kMac` | W5500 の MAC アドレス(同じ LAN の中で重ならない値) |
| `kPrivateKey` | このデバイスの秘密鍵(`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | トンネル内のこのデバイスのアドレス(既定 `10.9.0.2/24`) |
| `kServerPublicKey` | サーバーの公開鍵 |
| `kServerEndpoint` / `kServerPort` | サーバーの外側のアドレスとポート |
| `kServerTunnelIp` | トンネル内のサーバーのアドレス(既定 `10.9.0.1`) |
| `kNtpServer` | 時刻を合わせる NTP サーバー。W5500 経由で問い合わせる |

アドレスは DHCP で取ります。時刻は、起動時に W5500 経由の SNTP で 1 回合わせます。lwIP の `configTime()` は、lwIP から外に出る口がないので使えません。インターネットのない網では、網の中の NTP サーバーを指定します([初心者ガイド §8](../../docs/guide-beginner.ja.md#8-時刻について大事))。

サーバー側の設定は、[`Esp32Client`](../Esp32Client/README.ja.md#サーバー側の設定linux-の例) と同じです。

## 外側を回す方式

このサンプルは `wg.setOuterUdp(&outer, true)` で、`loop()` の `wg.update()` から W5500 を回します。`loop()` で `Ethernet.maintain()`(DHCP の更新)も呼ぶので、W5500 を使う処理を `loop()` にそろえるためです。この方式では、`loop()` を長くふさぐとトンネルも止まります。

MQTT や HTTP のように、相手の応答を待つ処理を使う場合は、`wg.setOuterUdp(&outer)`(第 2 引数なし)にします。ライブラリが専用のタスクで W5500 を回すので、`loop()` が待っていてもトンネルは動きます。その代わり、スケッチから W5500 を使ってはいけません。`Ethernet.maintain()` を呼ぶのをやめ、DHCP の更新が要らないよう固定アドレス(`Ethernet.begin(kMac, ip, dns, gateway, subnet)`)にします。

## 動かすと

シリアル(115200 bps)に次のように出ます。

```
TinyWG Esp32W5500Client
Local IP: 192.168.1.60
clock set
Public key: <このデバイスの公開鍵>
peer up: yes
```

サーバーで `nc -lu 9000` を実行すると、5 秒ごとにメッセージが届きます。`ping 10.9.0.2` にも返事があります。

## 注意

- まだ W5500 の実機では確かめていません(ビルドと、ESP32 の `WiFiUDP` を外側にした試験のみ)
- `W5500 not found` と出るときは、配線と `kCsPin` を確かめてください
