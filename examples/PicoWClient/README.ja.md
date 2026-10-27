# PicoWClient

[English](README.md) | [初心者ガイド](../../docs/guide-beginner.ja.md)

WireGuard サーバーに接続し、トンネル越しに 5 秒ごとに UDP の `hello from TinyWG` を `10.9.0.1:9000` に送ります。送信には、ふつうの UDP のクラスを使います。宛先がトンネルの中のアドレスなので、lwIP が `wg0` に流します。

## 必要なもの

- Raspberry Pi Pico W / Pico 2 W(arduino-pico 6.3 以降)
- WireGuard のサーバー(Linux の `wg-quick`、公式アプリなど)

## 設定する項目

| 定数 | 内容 |
|---|---|
| ネットワーク | Wi-Fi(`kWifiSsid` / `kWifiPass`) |
| `kPrivateKey` | このデバイスの秘密鍵(`wg genkey`) |
| `kTunnelIp` / `kTunnelMask` | トンネル内のこのデバイスのアドレス(既定 `10.9.0.4/24`) |
| `kServerPublicKey` | サーバーの公開鍵 |
| `kServerEndpoint` / `kServerPort` | サーバーの外側のアドレスとポート |
| `kServerTunnelIp` | トンネル内のサーバーのアドレス(既定 `10.9.0.1`) |

時刻は、`NTP.begin()` / `NTP.waitSet()` で合わせる。クライアントは、ハンドシェイクに時刻を入れるためです。

## サーバー側の設定(Linux の例)

```ini
[Interface]
PrivateKey = <サーバーの秘密鍵>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
PublicKey = <このデバイスの公開鍵(起動時にシリアルに表示される)>
AllowedIPs = 10.9.0.4/32
```

## 動かすと

シリアル(115200 bps)に次のように出ます。

```
TinyWG PicoWClient
Local IP: 192.168.1.50
Public key: <このデバイスの公開鍵>
peer up: yes
```

サーバーで `nc -lu 9000` を実行すると、5 秒ごとにメッセージが届きます。`ping 10.9.0.4` にも返事があります。

## 注意

- 今はビルドのみ確認しています。実機での確認はまだです
- arduino-pico の FreeRTOS モードには対応していません(コンパイルエラーになります)
- うまくいかないときは、[初心者ガイド](../../docs/guide-beginner.ja.md) §11 を見てください
