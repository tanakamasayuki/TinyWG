# API リファレンス

[English](api.md) | [初心者ガイド](guide-beginner.ja.md) | [上級者ガイド](guide-advanced.ja.md)

`#include <TinyWG.h>` で、ここにあるものがすべて使えます。

- [`TinyWGBase`](#tinywgbase): `TinyWG` と `TinyWGUdp` に共通のメソッド
- [`TinyWG`](#tinywg): lwIP のネットワークインターフェース `wg0`
- [`TinyWGUdp`](#tinywgudp): 任意の Arduino `UDP` の上で、生の IPv4 パケットを扱う
- [`tinywg::Device`](#tinywgdevice): プロトコルエンジン
- [型](#型): `SendResult`、`PeerStatus`、`Stats`、`Endpoint`、関数の型
- [鍵の関数](#鍵の関数)

鍵の文字列は、どれも `wg genkey` / `wg pubkey` / `wg genpsk` が出す 44 文字の Base64 です。

## TinyWGBase

### ピアの設定

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `int addPeer(const char *publicKeyBase64, const char *presharedKeyBase64 = nullptr)` | ピアの番号、失敗時は -1 | ピアを登録する。`begin()` の後で呼ぶ。-1 になるのは、`begin()` の前、鍵の文字列が不正、同じ公開鍵が登録済み、表が満杯(`TINYWG_MAX_PEERS`)のとき。X25519 を 1 回計算する(ESP32 で約 18 ms) |
| `int addPeer(const uint8_t publicKey[32], const uint8_t *presharedKey = nullptr)` | 同上 | 鍵をバイト列で渡す版 |
| `bool removePeer(int peer)` | 成否 | ピアを消し、鍵を消去する。空いた番号は次の `addPeer()` で再利用される |
| `bool addAllowedIp(int peer, const IPAddress &address, uint8_t cidr)` | 成否 | AllowedIPs を 1 つ足す(例: `IPAddress(10, 9, 0, 0), 24`)。ピアあたり `TINYWG_MAX_ALLOWED_IPS` 個まで。`cidr` は 0〜32 |
| `bool setEndpoint(int peer, const IPAddress &address, uint16_t port)` | 成否 | ハンドシェイクの送り先。相手から接続してくるだけのピアには不要。認証できたパケットを受け取るたびに、その送信元で上書きされる |
| `bool setPersistentKeepalive(int peer, uint16_t seconds)` | 成否 | keepalive の間隔(秒)。0 で止める。設定した時点で 1 つ送る。セッションがなく Endpoint があれば、ハンドシェイクが始まる |
| `bool connect(int peer)` | ハンドシェイクを送ったか | 今すぐハンドシェイクを始める。ふつうは最初の送信で自動的に始まる。false になるのは、Endpoint がない、前の開始から 5 秒以内、番号が不正のとき |

### MTU

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `virtual bool setMtu(uint16_t mtu)` | 成否 | 内側の MTU。576 〜 `TINYWG_MTU`。範囲外は false で、値は変わらない。いつでも呼べる。`TinyWG` では `wg0` の MTU も変わる |
| `uint16_t mtu() const` | 今の MTU | |

### 状態

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `bool peerUp(int peer)` | 今すぐ送れるセッションがあるか | `peerStatus()` の `sessionUp` |
| `bool peerStatus(int peer, tinywg::PeerStatus *out)` | 番号が有効か | ピアの状態を `out` に入れる([`PeerStatus`](#peerstatus)) |
| `tinywg::Stats stats()` | 統計 | デバイス全体の、捨てたパケットの数とハンドシェイクの数([`Stats`](#stats))。`begin()` で 0 に戻る |
| `void publicKey(char out[45]) const` | | このデバイスの公開鍵を Base64 で(44 文字 + NUL)。相手の `[Peer] PublicKey` に使う |

### 差し替え

| メソッド | 説明 |
|---|---|
| `void setRandom(tinywg::RandomFn fn)` | 暗号用の乱数源を差し替える。`begin()` より前に呼ぶ。既定の乱数源がないコアでは必須 |
| `void setWallClock(tinywg::WallClockFn fn)` | TAI64N に使う時刻の取得方法を差し替える。既定は `gettimeofday()` |
| `tinywg::Device &device()` | エンジンそのもの。ラッパーにない機能を使うとき。ESP32 の `TinyWG` では `LOCK_TCPIP_CORE()` で囲むこと |

## TinyWG

`TinyWGBase` を継承します。lwIP を持つコア(ESP32、arduino-pico、CH32V の Network ライブラリ)で使えます。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `bool begin(const IPAddress &localIp, const IPAddress &subnet, const char *privateKeyBase64, uint16_t listenPort = 51820)` | 成否 | `wg0` を立ち上げ、外側の UDP ポートを開く。`localIp` / `subnet` の範囲宛ての通信が `wg0` に流れる。`listenPort` を 0 にすると、空いているポートを使う(クライアント向け)。ピアの表と統計は消える。false になるのは、鍵の文字列が不正、乱数源がない、ポートが使えないとき |
| `bool begin(const IPAddress &localIp, const IPAddress &subnet, const uint8_t privateKey[32], uint16_t listenPort = 51820)` | 同上 | 鍵をバイト列で渡す版 |
| `void end()` | | `wg0` とポートを閉じ、鍵を消去する |
| `bool setMtu(uint16_t mtu)` | 成否 | `TinyWGBase::setMtu()` に加えて、`wg0` の MTU も変える |
| `struct netif *netif()` | `wg0` | 経路や NAPT を自分で設定するとき |
| `bool enableNapt(bool enable = true)` | 成否 | ESP32 のみ(`IP_NAPT` が有効なとき)。ピアから `wg0` に入り、ほかのインターフェースへ出ていく通信を NAPT で中継する。`begin()` の後で呼ぶ |

`loop()` から呼ぶものはありません。受信とタイマーは lwIP の中で動きます。ESP32 では、lwIP のコールバックの中から公開メソッドを呼ばないでください(デッドロックします)。

## TinyWGUdp

`TinyWGBase` を継承します。どのコアでも使えます。

| メソッド | 戻り値 | 説明 |
|---|---|---|
| `bool begin(UDP &udp, uint16_t listenPort, const char *privateKeyBase64)` | 成否 | `udp` を `listenPort` で開いて始める。ピアの表と統計は消える |
| `bool begin(UDP &udp, uint16_t listenPort, const uint8_t privateKey[32])` | 同上 | 鍵をバイト列で渡す版 |
| `void end()` | | `udp` を閉じ、鍵を消去する |
| `void onPacket(PacketHandler handler, void *context = nullptr)` | | 復号した内側の IPv4 パケットを受け取る関数を登録する |
| `tinywg::SendResult send(const uint8_t *packet, size_t len)` | [`SendResult`](#sendresult) | 内側の IPv4 パケットを 1 つ送る。送り先は宛先アドレスと AllowedIPs で決まる |
| `void update()` | | 受信したデータグラムを処理し、タイマーを進める。`loop()` から、100 ms より長く空けずに呼ぶ |

```cpp
typedef void (*PacketHandler)(int peer, uint8_t *packet, size_t len, void *context);
```

`packet` は書き換えて構いません。ハンドラの中から `send()` を呼んでも構いません。

## tinywg::Device

lwIP にも Arduino にも依存しないエンジンです。スレッド安全ではありません。すべての呼び出しを、呼ぶ側で直列にしてください。時刻 `nowMs` は `millis()` のような 32 ビットのミリ秒で、一周しても正しく扱います。

| メソッド | 説明 |
|---|---|
| `bool begin(const uint8_t privateKey[32], Transport *transport)` | 秘密鍵と出力先で始める。乱数源がなければ false |
| `void end()` | ピアと鍵を消去する |
| `void setRandom(RandomFn fn)` / `void setWallClock(WallClockFn fn)` | `TinyWGBase` と同じ |
| `int addPeer(...)` / `bool removePeer(int)` / `bool setEndpoint(int, const Endpoint &)` / `bool setPersistentKeepalive(int, uint16_t)` / `bool setMtu(uint16_t)` / `uint16_t mtu() const` | `TinyWGBase` と同じ |
| `bool addAllowedIp(int peer, const uint8_t addr[4], uint8_t cidr)` | アドレスをバイト列で渡す |
| `int findPeer(const uint8_t publicKey[32]) const` | 公開鍵からピアの番号を探す。なければ -1 |
| `int routePeer(const uint8_t addr[4]) const` | そのアドレス宛てに送るピア(AllowedIPs の最長一致)。なければ -1 |
| `bool peerStatus(int peer, PeerStatus *out) const` | ピアの状態 |
| `const Stats &stats() const` | 統計 |
| `const uint8_t *publicKey() const` | 公開鍵(32 バイト) |
| `void receiveOuter(uint8_t *data, size_t len, const Endpoint &from, uint32_t nowMs)` | 外側の UDP データグラムを 1 つ渡す。`data` はその場で復号される |
| `SendResult sendInner(const uint8_t *packet, size_t len, uint32_t nowMs)` | 内側の IPv4 パケットを 1 つ送る |
| `bool connect(int peer, uint32_t nowMs)` | 今すぐハンドシェイクを始める |
| `void tick(uint32_t nowMs)` | タイマーを進める。100 ms 程度ごとに呼ぶ |
| `static constexpr uint16_t kMinMtu = 576` | MTU の下限 |

### Transport

`Device` の出力先です。自分で実装して `begin()` に渡します。

```cpp
class Transport {
public:
    virtual void sendOuter(const Endpoint &to, const uint8_t *data, size_t len) = 0;
    virtual void deliverInner(int peer, uint8_t *packet, size_t len) = 0;
};
```

- `sendOuter`: 外側の UDP データグラムを `to` に送る
- `deliverInner`: 認証済みの内側の IPv4 パケット。`packet` は `receiveOuter()` に渡したバッファの一部で、書き換えてよい

## 型

### SendResult

| 値 | 意味 |
|---|---|
| `Sent` | 暗号化して送った |
| `Queued` | ハンドシェイクが済むまで保持した(`TINYWG_STAGED_PACKET`)。済んだら送る |
| `Dropped` | セッションがなく、保持もできなかった |
| `NoRoute` | 宛先がどのピアの AllowedIPs にも入っていない |
| `NoEndpoint` | 送り先がわからない(Endpoint が未設定で、相手からも届いていない) |
| `Invalid` | IPv4 でない、または MTU より大きい |

### PeerStatus

| 項目 | 型 | 意味 |
|---|---|---|
| `sessionUp` | `bool` | 今すぐ送れるセッションがある |
| `handshakeInFlight` | `bool` | 送ったハンドシェイクの返事を待っている |
| `hasHandshake` | `bool` | 一度でもハンドシェイクが済んだ |
| `lastHandshakeMs` | `uint32_t` | 最後にハンドシェイクが済んだときの時刻(ミリ秒) |
| `endpoint` | `Endpoint` | 今の送り先 |
| `rxBytes` / `txBytes` | `uint64_t` | 外側の UDP のバイト数(WireGuard の頭とタグを含む) |

### Stats

すべて `uint32_t` です。項目の意味と主な原因は、[上級者ガイド §10](guide-advanced.ja.md#10-状態の監視と診断) にあります。

`rxShort`、`rxBadMac1`、`rxHandshakeFail`、`rxUnknownPeer`、`rxStaleTimestamp`、`rxFlood`、`rxNoSession`、`rxDecryptFail`、`rxReplay`、`rxNotAllowed`、`rxNotIpv4`、`handshakesInitiated`、`handshakesCompleted`、`handshakesResponded`

### Endpoint

```cpp
struct Endpoint {
    uint8_t addr[4];   // IPv4 アドレス
    uint16_t port;     // 0 = 未設定
};
```

### 関数の型

```cpp
// out に len バイトの暗号用乱数を入れる。使えないときは false
typedef bool (*RandomFn)(uint8_t *out, size_t len);
// 今の時刻を Unix 秒とナノ秒で返す。再起動をまたいで戻らないことが大事
typedef void (*WallClockFn)(uint64_t *unixSeconds, uint32_t *nanoseconds);
```

## 鍵の関数

名前空間 `tinywg` にあります。

| 関数 | 説明 |
|---|---|
| `bool generatePrivateKey(uint8_t privateKey[32], RandomFn random = nullptr)` | 秘密鍵を作る。`random` を省くと既定の乱数源を使う。乱数源がなければ false |
| `void derivePublicKey(uint8_t publicKey[32], const uint8_t privateKey[32])` | 秘密鍵から公開鍵を計算する |
| `bool keyFromBase64(uint8_t key[32], const char *base64)` | Base64 からバイト列へ。不正な文字列なら false |
| `void keyToBase64(char out[45], const uint8_t key[32])` | バイト列から Base64 へ(44 文字 + NUL) |

## ビルド時の設定

`-D` で上書きできます。値と RAM への影響は [上級者ガイド §8](guide-advanced.ja.md#8-mtu-とメモリ) にあります。

`TINYWG_MAX_PEERS`(4)、`TINYWG_MAX_ALLOWED_IPS`(4)、`TINYWG_MTU`(1420)、`TINYWG_REPLAY_WINDOW`(128)、`TINYWG_STAGED_PACKET`(1)

バージョンは `TINYWG_VERSION_MAJOR` / `TINYWG_VERSION_MINOR` / `TINYWG_VERSION_PATCH` / `TINYWG_VERSION_STR`(`tinywg_version.h`)で参照できます。
