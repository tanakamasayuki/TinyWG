# Ch32EthernetClient

[English](README.md) | [初心者ガイド](../../docs/guide-beginner.ja.md)

WireGuard サーバーに接続し、トンネル越しに 5 秒ごとに UDP の `hello from TinyWG` を `10.9.0.1:9000` に送ります。送信には、ふつうの UDP のクラスを使います。宛先がトンネルの中のアドレスなので、lwIP が `wg0` に流します。

## 必要なもの

- Ethernet を内蔵した CH32V(CH32V307 など。ArduinoCore-CH32RV 0.4 以降、Network ライブラリ)
- WireGuard のサーバー(Linux の `wg-quick`、公式アプリなど)

## 設定する項目

| 定数 | 内容 |
|---|---|
| ネットワーク | Ethernet(`ETH.begin()`、DHCP)。設定する項目はない |
| `kPrivateKey` | このデバイスの秘密鍵(`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | トンネル内のこのデバイスのアドレス(既定 `10.9.0.3/24`) |
| `kServerPublicKey` | サーバーの公開鍵 |
| `kServerEndpoint` / `kServerPort` | サーバーの外側のアドレスとポート |
| `kServerTunnelIp` | トンネル内のサーバーのアドレス(既定 `10.9.0.1`) |

時刻は、`configTime()` で NTP に合わせる(最大 10 秒待つ)。クライアントは、ハンドシェイクに時刻を入れるためです。

## サーバー側の設定(Linux の例)

```ini
[Interface]
PrivateKey = <サーバーの秘密鍵>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <このデバイスの公開鍵(起動時にシリアルに表示される)>
AllowedIPs = 10.9.0.3/32
```

## 動かすと

シリアル(115200 bps)に次のように出ます。

```
TinyWG Ch32EthernetClient
Local IP: 192.168.1.50
Public key: <このデバイスの公開鍵>
peer up: yes
```

サーバーで `nc -lu 9000` を実行すると、5 秒ごとにメッセージが届きます。`ping 10.9.0.3` にも返事があります。

## 注意

- 乱数に TRNG を使います。TRNG のない系列(CH32V208 など)では `wg.begin()` が失敗するので、`wg.setRandom()` で乱数源を渡してください([上級者ガイド](../../docs/guide-advanced.ja.md) §7)
- lwIP は `loop()` の後と `delay()` の中で動きます。`loop()` を長くふさがないでください
- CH32V307 の実機で、トンネル越しの通信と長時間試験を確かめています
- うまくいかないときは、[初心者ガイド](../../docs/guide-beginner.ja.md) §11 を見てください
