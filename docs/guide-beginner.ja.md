# 初心者ガイド

[English](guide-beginner.md) | [上級者ガイド](guide-advanced.ja.md) | [README](../README.ja.md)

このガイドでは、ESP32 と Linux の PC を WireGuard でつなぎ、トンネル越しに通信できたことを確かめます。Arduino でスケッチを書き込んだことがあれば十分です。WireGuard の知識は要りません。

所要時間は 30 分ほどです。

## 1. WireGuard と TinyWG

WireGuard は、2 台の機器のあいだに暗号化された「トンネル」をつくる仕組みです。トンネルの中では、それぞれの機器に専用の IP アドレス(例: `10.9.0.1` と `10.9.0.2`)が割り当てられます。その宛先に送った通信は、自動で暗号化されて相手に届きます。

```
  PC (10.9.0.1)                                   ESP32 (10.9.0.2)
     │                                                  │
     └──── 暗号化された UDP(家の LAN やインターネット) ────┘
```

TinyWG は、この WireGuard をマイコンで動かすライブラリです。相手は、Linux の WireGuard、Windows / macOS / スマートフォンの公式アプリなど、ふつうの WireGuard ならどれでも構いません。

ESP32 で使う場合、TinyWG はネットワークのインターフェース `wg0` として動きます。そのため、ふだん使っている `WiFiClient` や `WiFiUDP` で、トンネルの中の相手(`10.9.0.1` など)と、そのまま通信できます。

## 2. 用意するもの

- **ESP32 のボード**: ESP32 または ESP32-S3。この 2 つは実機で確かめています。CH32V307(Ethernet)と Pico W も使えます。その場合は [§9](#9-esp32-以外のボード) を見てください
- **Arduino IDE 2** または `arduino-cli`
  - ESP32 のボードパッケージ(arduino-esp32)3.3 以降が入っていること
- **Linux の PC**: Ubuntu など。`wireguard-tools` を入れます(`sudo apt install wireguard-tools`)
  - Windows や macOS でも、公式アプリで同じことができます。設定の中身は §5 と同じです
- ESP32 と PC が、同じ Wi-Fi(LAN)につながっていること

## 3. ライブラリを入れる

まだライブラリマネージャーには登録していません。GitHub から入れます。

**Arduino IDE の場合**: GitHub のページから ZIP をダウンロードし、「スケッチ」→「ライブラリをインクルード」→「.ZIP 形式のライブラリをインストール」で入れます。

**git の場合**: Arduino のライブラリのフォルダに clone します。

```sh
cd ~/Arduino/libraries
git clone https://github.com/tanakamasayuki/TinyWG.git
```

入ると、「ファイル」→「スケッチ例」→「TinyWG」にサンプルが並びます。

## 4. 鍵をつくる

WireGuard では、機器ごとに「秘密鍵」と「公開鍵」の組を 1 つずつ持ちます。

- **秘密鍵**: その機器だけが知っている鍵です。ほかに渡してはいけません
- **公開鍵**: 秘密鍵から計算される鍵です。相手に渡して、相手の設定に書いてもらいます

PC で、PC 用と ESP32 用の 2 組をつくります。

```sh
wg genkey | tee pc.key | wg pubkey > pc.pub
wg genkey | tee esp32.key | wg pubkey > esp32.pub
cat pc.key pc.pub esp32.key esp32.pub
```

1 行ずつ、44 文字の文字列が 4 つ表示されます(例: `yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=`)。この 4 つを、このあとの設定で使います。

| ファイル | 使う場所 |
|---|---|
| `pc.key` | PC の設定の `PrivateKey` |
| `pc.pub` | ESP32 のスケッチの `kServerPublicKey` |
| `esp32.key` | ESP32 のスケッチの `kPrivateKey` |
| `esp32.pub` | PC の設定の `[Peer]` の `PublicKey` |

自分の秘密鍵と、相手の公開鍵を組み合わせて使います。ここを取り違えると、つながりません。

## 5. PC 側(WireGuard サーバー)を設定する

`/etc/wireguard/wg0.conf` を次の内容で作ります。`<...>` の部分は、§4 でつくった鍵に置き換えてください。

```ini
[Interface]
PrivateKey = <pc.key の中身>
Address = 10.9.0.1/24
ListenPort = 51820

[Peer]
# ESP32
PublicKey = <esp32.pub の中身>
AllowedIPs = 10.9.0.2/32
```

- `Address`: トンネルの中での PC のアドレスです
- `ListenPort`: PC が WireGuard を待ち受ける UDP のポートです
- `AllowedIPs`: このピア(ESP32)が使ってよいトンネル内のアドレスです

トンネルを起動し、PC の LAN の IP アドレスを確かめます。

```sh
sudo wg-quick up wg0
ip -4 addr show     # 例: 192.168.1.10。ESP32 のスケッチで使う
```

ファイアウォール(ufw)を使っている場合は、次の 2 つを許可します。WireGuard の UDP と、トンネルの中から PC に届く通信です。

```sh
sudo ufw allow 51820/udp
sudo ufw allow in on wg0
```

## 6. ESP32 側のスケッチ

Arduino IDE で「スケッチ例」→「TinyWG」→「Esp32Client」を開き、先頭の設定を書き換えます。

```cpp
static const char *kWifiSsid = "your-ssid";          // Wi-Fi の名前
static const char *kWifiPass = "your-passphrase";    // Wi-Fi のパスワード

static const char *kPrivateKey = "<esp32.key の中身>";
static const IPAddress kTunnelIp(10, 9, 0, 2);       // PC の AllowedIPs と同じ
static const IPAddress kTunnelMask(255, 255, 255, 0);

static const char *kServerPublicKey = "<pc.pub の中身>";
static const IPAddress kServerEndpoint(192, 168, 1, 10);   // PC の LAN の IP
static constexpr uint16_t kServerPort = 51820;             // PC の ListenPort
static const IPAddress kServerTunnelIp(10, 9, 0, 1);       // PC の Address
```

スケッチの中では、次の順に処理しています。

1. Wi-Fi につなぐ
2. NTP で時刻を合わせる(理由は §8 を参照)
3. `wg.begin()` でトンネルのインターフェース `wg0` を立ち上げる
4. `wg.addPeer()` で相手(PC)を登録し、`addAllowedIp()`、`setEndpoint()`、`setPersistentKeepalive()` を設定する
5. `loop()` で 5 秒ごとに、`WiFiUDP` で `10.9.0.1:9000` に `hello from TinyWG` を送る

5 番目で、トンネル用の特別な関数は使っていません。宛先がトンネルの中のアドレスなので、ESP32 のネットワーク機能(lwIP)が自動で `wg0` に流します。

書き込んだら、シリアルモニタを 115200 bps で開きます。

## 7. つながったか確かめる

シリアルモニタに `peer up: yes` が出れば、ハンドシェイク(鍵の交換)が済んでいます。

PC からも確かめます。

```sh
sudo wg show                 # "latest handshake: ... seconds ago" が出ていればつながっている
ping 10.9.0.2                # ESP32 がトンネルの中から返事をする
nc -lu 9000                  # ESP32 から 5 秒ごとに "hello from TinyWG" が届く
```

ここまで見えれば完成です。ESP32 と PC のあいだの通信は、すべて暗号化されています。

## 8. 時刻について(大事)

TinyWG から接続を始める側(クライアント)は、ハンドシェイクのたびに今の時刻を相手に送ります。相手は、前回より古い時刻のハンドシェイクを「再送攻撃かもしれない」として受け付けません。

マイコンの多くは、電源を入れ直すと時計が 1970 年に戻ります。そのまま接続すると、2 回目の起動から先は相手に断られてしまいます。サンプルが `wg.begin()` の前に NTP で時刻を合わせているのは、このためです。**クライアントとして使うときは、必ず先に時刻を合わせてください。**

相手から接続されるのを待つだけ(サーバー)なら、時刻は要りません。

## 9. ESP32 以外のボード

| ボード | サンプル | 違い |
|---|---|---|
| CH32V307 など(Ethernet) | `Ch32EthernetClient` | Wi-Fi の代わりに `ETH.begin()`。UDP は `NetworkUDP` |
| Raspberry Pi Pico W / Pico 2 W | `PicoWClient` | 時刻合わせは `NTP.begin()`。今はビルドのみ確認 |

スケッチの組み立ては、どれも同じです。

## 10. 次にやってみること

### ESP32 をサーバーにする

`Esp32Server` のサンプルでは、ESP32 が WireGuard のサーバーになります。PC やスマートフォンが ESP32 に接続し、トンネル越しに TCP のエコーサーバー(ポート 7)を使えます。

ESP32 には、相手の公開鍵と、相手のトンネル内のアドレスだけを登録します。相手の IP アドレスは要りません。接続してきたパケットから、自動で覚えます。

相手(PC やスマートフォンの公式アプリ)の設定は、次のようになります。

```ini
[Interface]
PrivateKey = <相手の秘密鍵>
Address = 10.9.0.2/24

[Peer]
PublicKey = <ESP32 の公開鍵(起動時にシリアルに表示される)>
Endpoint = <ESP32 の LAN の IP>:51820
AllowedIPs = 10.9.0.1/32
PersistentKeepalive = 25
```

接続できたら、PC から `nc 10.9.0.1 7` を実行します。打った文字がそのまま返ってくれば成功です。

### MQTT や HTTP を通す

TinyWG の `wg0` は、ふつうのネットワークインターフェースです。`PubSubClient` などの MQTT クライアントや、HTTP のクライアント・サーバーも、宛先をトンネル内のアドレスにするだけでトンネルを通ります。

## 11. うまくいかないとき

まず、状態をシリアルに出してみます。

```cpp
tinywg::Stats s = wg.stats();
Serial.printf("init=%u done=%u badmac=%u unknown=%u hsfail=%u stale=%u notallowed=%u\n",
              (unsigned)s.handshakesInitiated, (unsigned)s.handshakesCompleted, (unsigned)s.rxBadMac1,
              (unsigned)s.rxUnknownPeer, (unsigned)s.rxHandshakeFail, (unsigned)s.rxStaleTimestamp,
              (unsigned)s.rxNotAllowed);
```

| 症状 | 考えられる原因 |
|---|---|
| `wg.begin()` が false | 秘密鍵の文字列が壊れている(44 文字の Base64 か確認)。乱数源のないチップ(→ 上級者ガイド) |
| `addPeer()` が -1 | `begin()` より前に呼んでいる。公開鍵の文字列が壊れている。同じ鍵を 2 回登録した。ピアが上限(既定 4)に達した |
| `init` だけ増え、`done` が増えない | 相手に届いていないか、相手が断っている。Endpoint の IP とポート、PC のファイアウォール(`51820/udp`)、PC の `[Peer]` の `PublicKey` を確認する |
| PC の `sudo wg show` に何も出ない | ESP32 からのパケットが PC に届いていない。同じ LAN にいるか、Endpoint が PC の IP になっているか |
| 2 回目の起動からつながらない | 時刻が合っていない(§8)。相手は古い時刻のハンドシェイクを黙って捨てる |
| `badmac` が増える | 相手が、こちらの公開鍵を間違えて設定している |
| `unknown` が増える | 登録していない公開鍵の相手が接続してきた(こちらの `addPeer()` の鍵が違う) |
| `notallowed` が増える | 相手のトンネル内のアドレスが、こちらの `addAllowedIp()` の範囲に入っていない |
| つながるが ping が返らない | PC のファイアウォールが `wg0` を止めている(`sudo ufw allow in on wg0`) |

## 12. 用語

| 用語 | 意味 |
|---|---|
| ピア(Peer) | トンネルの相手 1 台 |
| エンドポイント(Endpoint) | 相手の「外側」の IP アドレスとポート。暗号化した UDP の送り先 |
| AllowedIPs | そのピアに送る宛先、かつ、そのピアから受け取ってよい送信元のアドレスの範囲 |
| ハンドシェイク | 鍵を交換してセッションを作る手順。通信が続いていれば 2 分ごとに自動でやり直す |
| Keepalive | 送るものがないときに出す空のパケット。NAT やルーターに経路を忘れさせないために使う |
| MTU | トンネルの中で一度に送れるパケットの大きさ(既定 1420 バイト) |

もっと細かい設定や、仕組みの詳細は [上級者ガイド](guide-advanced.ja.md) にあります。
