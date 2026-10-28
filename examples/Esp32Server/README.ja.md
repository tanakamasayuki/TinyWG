# Esp32Server

[English](README.md) | [初心者ガイド](../../docs/guide-beginner.ja.md)

ESP32 を、2 ピア向けの WireGuard サーバーにします。トンネルの中だけで届く TCP のエコーサーバー(ポート 7)を動かし、10 秒ごとに各ピアの状態をシリアルに出します。

## 必要なもの

- ESP32 / ESP32-S3 などの ESP32 系(arduino-esp32 3.3 以降)
- 接続してくる WireGuard のピア(Linux の `wg-quick`、PC やスマートフォンの公式アプリなど)

## 設定する項目

| 定数 | 内容 |
|---|---|
| `kWifiSsid` / `kWifiPass` | Wi-Fi |
| `kPrivateKey` | このサーバーの秘密鍵(`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | トンネル内のサーバーのアドレス(既定 `10.9.0.1/24`) |
| `kListenPort` | 待ち受ける UDP のポート(既定 51820) |
| `kPeers` | ピアの公開鍵と、トンネル内のアドレス(既定 `10.9.0.2` と `10.9.0.3`) |
| `kTimestampReset` | 時計のないクライアントを、最後の通信からこの秒数で受け付ける(既定 30。0 で標準どおり受け付けない) |
| `kTimeServer` | LAN のアドレスの UDP 123 番で、このサーバーの時刻を NTP で配る(既定 true) |

ピアの IP アドレス(Endpoint)は要りません。最初に届いたハンドシェイクの送信元を覚え、相手が移動しても追従します。

サーバーの時計は接続に関係しません。起動時に `pool.ntp.org` で時刻を合わせようとし、インターネットにつながらなければ 1970 年から数えます。クライアントは、NTP の問い合わせ先にこのサーバーの LAN のアドレスを使えます。正しい時刻になるのは、サーバーがインターネットで時刻を合わせられたときだけです([初心者ガイド §8](../../docs/guide-beginner.ja.md#8-時刻について大事))。

## ピア側の設定

```ini
[Interface]
PrivateKey = <ピアの秘密鍵>
Address = 10.9.0.2/24

[Peer]
PublicKey = <サーバーの公開鍵(起動時にシリアルに表示される)>
Endpoint = <サーバーの LAN の IP>:51820
AllowedIPs = 10.9.0.1/32
PersistentKeepalive = 25
```

2 台目のピアは、`Address` を `10.9.0.3/24` にします。

## 動かすと

```
TinyWG Esp32Server
Endpoint: 192.168.1.50:51820
Public key: <サーバーの公開鍵>
peer 0: up rx=1234 tx=1180
peer 1: down rx=0 tx=0
```

ピアから `nc 10.9.0.1 7` で接続すると、打った文字がそのまま返ってきます。`ping 10.9.0.1` にも返事があります。

## 発展

- `wg.enableNapt()` を足すと、ピアがサーバーを経由して家の LAN やインターネットに出られます。ピア側の AllowedIPs に LAN の範囲を加えてください([上級者ガイド](../../docs/guide-advanced.ja.md) §4)
- ピアを 4 台より多くするときは、ビルド時に `TINYWG_MAX_PEERS` を増やします
- インターネットから直接届くところに置く場合は、上級者ガイド §13 のセキュリティ上の注意を読んでください
