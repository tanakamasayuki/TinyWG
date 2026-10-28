# 上級者ガイド

[English](guide-advanced.md) | [初心者ガイド](guide-beginner.ja.md) | [API リファレンス](api.ja.md) | [README](../README.ja.md) | [仕様書](../SPEC.ja.md)

TinyWG の構成、API の細かい振る舞い、運用で気をつける点をまとめます。[初心者ガイド](guide-beginner.ja.md)の内容は済んでいるものとして書いています。

## 1. 構成

```
アプリ(WiFiClient、MQTT、HTTP、生の IPv4 ...)
   │
   ├─ TinyWG      lwIP のネットワークインターフェース wg0(ESP32 / Pico W / CH32V)
   ├─ TinyWGUdp   任意の Arduino UDP の上。内側は生の IPv4 パケット
   │
tinywg::Device    プロトコルエンジン(ハンドシェイク、セッション、タイマー、AllowedIPs)
   │              lwIP にも Arduino にも依存しない
暗号              Monocypher(X25519、ChaCha20-Poly1305)、BLAKE2s(RFC 7693)
```

| フロントエンド | 使いどころ | `loop()` から呼ぶもの |
|---|---|---|
| `TinyWG` | lwIP を持つコアで、既存のソケットのコードをそのままトンネルに通したいとき | なし(lwIP の中で動く) |
| `TinyWGUdp` | lwIP がない、または内側のパケットを自分で組み立て・解釈したいとき | `wg.update()` |
| `tinywg::Device` | UDP の手段が Arduino の `UDP` ではないとき(セルラーモデムの AT コマンドなど) | `receiveOuter()` と `tick()` |

どれも、動的なメモリ確保をしません。バッファはすべて、オブジェクトの中に静的に持ちます。

## 2. 実行コンテキストと排他

`TinyWG` では、次の 3 つがすべて lwIP のコンテキストで動きます。

- 外側の UDP の受信
- `wg0` からの送信
- 100 ms ごとのタイマー(`sys_timeout`)

スケッチから呼ぶ公開メソッドは、コアごとの方法で lwIP と排他します。

| コア | 排他 | 注意 |
|---|---|---|
| ESP32 | `LOCK_TCPIP_CORE()` | lwIP のコールバックの中(`WiFi.onEvent` の一部、raw API のコールバック)から公開メソッドを呼ぶとデッドロックする |
| arduino-pico | `LWIPMutex` | FreeRTOS モードは未対応(コンパイルエラーにしている) |
| CH32V | なし | lwIP は `loop()` が 1 回終わるたびと、`delay()` の中でしか動かない。`loop()` を長くふさぐと、ハンドシェイクもタイマーも止まる |

`wg.device()` でエンジンを直接触るときは、ESP32 では自分で `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()` で囲んでください。

`TinyWGUdp` と `Device` は、スレッドを前提にしていません。すべての呼び出しを同じタスクから行ってください。

### 外側の UDP を差し替える

W5500 を Arduino の `Ethernet` ライブラリで使う場合や、SIM7080G などのモデムを AT コマンドで使う場合、TCP/IP はモジュールの中で動きます。lwIP の外なので、そのままでは `wg0` の外側の通信がモジュールから出ていきません。

`setOuterUdp()` を使うと、`wg0` は lwIP に置いたまま、外側の UDP だけをモジュールの `UDP` にできます。トンネルの中では、`NetworkClient` や MQTT など lwIP 上のコードがそのまま使えます。

```cpp
EthernetUDP outer;
TinyWG wg;

wg.setOuterUdp(&outer);                                     // begin() の前
wg.begin(kTunnelIp, kTunnelMask, kPrivateKey, 51820);      // outer を 51820 番で開く
```

外側の送受信を回すのは、既定ではライブラリ自身です。

| コア | 外側を回すもの | 注意 |
|---|---|---|
| ESP32 | 専用の FreeRTOS タスク(優先度 2、1 ms ごと) | `loop()` がソケットの送信や MQTT の接続で待っていても、トンネルは止まらない。その代わり、スケッチから同じモジュールを同時に使わない(トンネルの外の `EthernetClient` など) |
| CH32V | lwIP のタイマー(1 ms ごと) | lwIP のタイマーは `loop()` と同じ流れで動くので、ブロックする呼び出しの中でも回る |
| arduino-pico | `loop()` の `wg.update()` | 自動では回らない |

スケッチからもモジュールを直接使う場合は、`wg.setOuterUdp(&outer, true)` とし、`loop()` で `wg.update()` を呼びます。この場合、`loop()` が長く止まると、その間トンネルも止まります。ソケットの送信のように、相手の応答を待つ呼び出しは使えません(ESP32 の `WiFiClient::write()` は ACK を最大 10 秒待つので、その間に通信が詰まります)。`update()` は、どの方式でも呼んで害はありません。

- `wg0` から出るパケットは、いったんキュー(8 個)にためてから、外側を回す処理がモジュールに渡します。あふれた分は捨てます(UDP と同じ扱い)
- 外側が lwIP を通らないので、「外側の宛先が `wg0` 経由になるなら送らない」というループ防止はしません。Wi-Fi を使っていなければ、`wg0` をデフォルトルートにもできます
- 試験では、ESP32 の `WiFiUDP` を外側に渡して確かめています。W5500 と SIM7080G の実機では、まだ確かめていません

## 3. 設定の順序と寿命

```cpp
wg.setRandom(...);       // 任意。begin() より前(begin() で使う)
wg.setWallClock(...);    // 任意。いつでも
wg.setMtu(1280);         // 任意。いつでも
wg.begin(ip, mask, privateKey, listenPort);
int p = wg.addPeer(publicKey, psk);   // begin() の後
wg.addAllowedIp(p, ...);
wg.setEndpoint(p, ...);               // 任意
wg.setPersistentKeepalive(p, 25);     // 任意
```

- `begin()` は、毎回ピアの表と統計を消してから始めます。鍵を変えるときは `begin()` からやり直し、ピアを登録し直してください
- `addPeer()` は `begin()` の後でないと -1 を返します。内部で X25519 を 1 回計算するので、1 ピアあたり ESP32 で約 18 ms、CH32V307 で約 24 ms かかります
- `removePeer()` で空いた番号は、次の `addPeer()` で再利用されます。番号を覚えているコードに注意してください
- `setRandom()`、`setWallClock()`、`setMtu()` の設定は、`begin()` / `end()` をまたいで残ります
- `setPersistentKeepalive()` は、設定した時点で keepalive を 1 つ送ります(Linux と同じ)。セッションがなく Endpoint があれば、ハンドシェイクが始まります

## 4. ルーティング

### AllowedIPs(クリプトキールーティング)

`addAllowedIp()` で登録した範囲は、2 つの意味を持ちます。

- **送信:** 宛先を最長一致で調べ、送り先のピアを決める
- **受信:** 復号したパケットの送信元が、そのピアの範囲に入っているかを確かめる。入っていなければ捨てて、`rxNotAllowed` を数える

ピアあたりの数は `TINYWG_MAX_ALLOWED_IPS`(既定 4)までです。

### lwIP から wg0 に流れるもの

lwIP には、ネットワークインターフェースのサブネットと既定経路しかありません。`wg0` に流れるのは、`begin()` で渡したアドレスとマスクの範囲(例: `10.9.0.0/24`)宛ての通信だけです。

AllowedIPs に `192.168.50.0/24` を書いても、lwIP がその宛先を `wg0` に流さなければ、トンネルには入りません。トンネルの向こうに複数のネットワークがあるときは、`begin()` のマスクを広くとるのが簡単です。

`netif_set_default(wg.netif())` で `wg0` を既定経路にすることもできます。ただし、外側の UDP の宛先(Endpoint)までトンネルに入る場合は、ループを防ぐために送信しません。つまり既定経路にできるのは、Endpoint が直接つながったサブネット(同じ LAN)にあるときだけです。

### NAPT(ESP32)

`wg.enableNapt()` を呼ぶと、ピアから `wg0` に入り、ほかのインターフェース(Wi-Fi など)へ出ていく通信を、ESP32 の LAN 側のアドレスに変換して中継します。外からトンネルで ESP32 に入り、そこから家の LAN やインターネットに出る使い方です。

ピア側の設定では、AllowedIPs に LAN の範囲も入れます。

```ini
[Peer]
PublicKey = <ESP32 の公開鍵>
Endpoint = <ESP32 に届くアドレス>:51820
AllowedIPs = 10.9.0.0/24, 192.168.1.0/24
```

ESP32 側は、ピアのトンネル内アドレス(`10.9.0.2/32` など)だけを AllowedIPs に入れれば十分です。LAN からの返事は、NAPT がピアのアドレスに戻してから `wg0` に流します。

### ローミング

認証できたパケットを受け取るたびに、そのピアの Endpoint を送信元に更新します。相手の IP やポートが変わっても(NAT の割り当て直し、回線の切り替え)、相手から 1 パケット届けば追従します。

こちらの IP が変わった場合も同じです。こちらから何か送れば(persistent keepalive を含む)、相手が追従します。

## 5. 鍵の管理

### デバイスの中で鍵を作り、保存する(ESP32)

ソースに秘密鍵を書く代わりに、初回の起動時に作って NVS に保存できます。公開鍵はシリアルに出して、相手の設定に登録します。

```cpp
#include <Preferences.h>

uint8_t key[32];
Preferences prefs;
prefs.begin("tinywg");
if (prefs.getBytes("key", key, sizeof(key)) != sizeof(key)) {
    tinywg::generatePrivateKey(key);   // 既定の乱数源(esp_fill_random)を使う
    prefs.putBytes("key", key, sizeof(key));
}
prefs.end();
wg.begin(ip, mask, key, 51820);
memset(key, 0, sizeof(key));

char pub[45];
wg.publicKey(pub);
Serial.println(pub);   // 相手の [Peer] PublicKey に使う
```

- NVS も Flash も、そのままでは読み出せます。物理的に触られる機器では、ESP32 の Flash 暗号化や NVS 暗号化を検討してください
- 鍵の変換には `tinywg::keyFromBase64()` / `keyToBase64()` / `derivePublicKey()` が使えます

### 事前共有鍵(PSK)

`addPeer(publicKey, psk)` の 2 番目の引数に、`wg genpsk` で作った鍵を渡します。相手の `PresharedKey` と同じ値にしてください。違っているとハンドシェイクが失敗し、`rxHandshakeFail` が増えます。

## 6. 時刻(TAI64N)

ハンドシェイクを始める側は、TAI64N の時刻を送ります。受け手は、そのピアから前に受け取った値より大きいものしか受け付けません。

TinyWG は、1 回の起動の中では、時刻が戻っても送る値が単調に増えるようにしています。気をつけるのは再起動をまたぐ場合です。

| 方法 | 向いている場面 |
|---|---|
| NTP(既定の `gettimeofday()`) | ネットにつながってから `begin()` できる |
| RTC | 電池つきの RTC がある。`setWallClock()` で RTC を読む関数を渡す |
| 起動回数 | NTP も RTC もない(下の例) |

時刻の代わりに、起動回数を不揮発領域に数える例です(ESP32)。

```cpp
#include <Preferences.h>
#include <esp_timer.h>

static uint32_t bootCount;

// 上位に起動回数、下位に起動からの秒数。2106 年までの実時刻(32 ビットに収まる)より必ず大きい
static void bootClock(uint64_t *seconds, uint32_t *nanoseconds)
{
    uint64_t us = esp_timer_get_time();
    *seconds = ((uint64_t)bootCount << 32) + us / 1000000;
    *nanoseconds = (uint32_t)(us % 1000000) * 1000;
}

void setup()
{
    Preferences prefs;
    prefs.begin("tinywg");
    bootCount = prefs.getUInt("boot", 0) + 1;
    prefs.putUInt("boot", bootCount);
    prefs.end();
    wg.setWallClock(bootClock);
    // ...
}
```

この方法に一度切り替えると、相手は大きな値を覚えます。そのため、あとから NTP の実時刻に戻すと拒否されます。戻すときは、相手の側でそのピアを登録し直してください。

相手から接続されるだけの側(レスポンダ)は、時刻を使いません。

### 時計のないクライアントを受け付ける

TinyWG が接続を受ける側なら、`setTimestampReset()` で、しばらく何も届いていないピアの時刻の記録を忘れさせられます。閉域網などで、クライアントに NTP も RTC もない場合に使います。

```cpp
wg.setTimestampReset(30);   // 30 秒、応答もデータも届いていないピアは、古い時刻でも受け付ける
```

- 既定は 0 で、標準の WireGuard と同じく忘れません
- 「届いた」に数えるのは、認証できたハンドシェイク応答とデータ(キープアライブを含む)だけです。ハンドシェイク開始は再送されたものかもしれないので数えません
- 再起動したクライアントは、最後に通信してからこの秒数がたつとつながります。クライアントはハンドシェイクを 5 秒おきに 90 秒まで送り直すので、90 秒より短くしておけば、その間につながります
- キープアライブを使うなら、その間隔より長くします。短いと、正常に通信しているピアまで、キープアライブの合間に「何も届いていない」とみなされます

その代わり、再送攻撃に対して弱くなる範囲があります。盗聴しておいたハンドシェイク開始を送り直されると、何も届いていない間のピアについては受け付けてしまいます。その結果、そのピアへの宛先が一時的に書き換わったり、組み立て中のハンドシェイクが壊れたりして、通信が止まることがあります。鍵が漏れることや、なりすまし、データの注入はありません。通信中のピアは、今までどおり再送を拒否します。

相手が Linux や公式アプリのときは、そちらにこの設定はありません。クライアントの時刻を合わせてください。

### 簡易 NTP サーバー(`TinyWGTimeServer`)

WG サーバーの LAN のアドレスで、自分の時計の時刻を NTP(SNTP)で答えます。クライアントは、WG の接続先と同じアドレスを NTP の問い合わせ先にできます。どの網でどこに合わせるかは[初心者ガイド §8.2](guide-beginner.ja.md#82-ntp-でどこに合わせるか)にあります。

```cpp
WiFiUDP ntpUdp;               // NTP 専用の UDP。TinyWGUdp と同じものは使わない
TinyWGTimeServer timeServer;

void setup()
{
    // ...
    configTime(0, 0, "pool.ntp.org");   // つながれば実時刻、つながらなければ 1970 年から
    timeServer.begin(ntpUdp);           // UDP 123 番
}

void loop()
{
    timeServer.update();                // 届いた問い合わせに答える
    // ...
}
```

- 答える時刻は既定で `gettimeofday()` です(TinyWG がハンドシェイクに使う時計と同じ)。`setClock()` で RTC を読む関数などに差し替えられます
- 応答は stratum 10、参照 ID は `LOCL` です(chrony の `local stratum 10` と同じ扱い)。時計が合っていなくても、うるう秒の警告(未同期の印)は付けません。付けると受け付けないクライアントがあるためです
- `update()` は `loop()` から呼びます。長く止まる処理の中でも呼ぶと、応答が遅れません
- 使わなければコストはかかりません。インスタンスを作らなければ、Flash も RAM も増えません(ESP32 と CH32V307 で確認)

NTP の応答には認証がありません。同じ網にいる攻撃者は、遠い未来の時刻を返せます。そうすると、クライアントのハンドシェイクの時刻が未来に飛び、時刻が正しく戻ったあとの接続が拒否されます。TinyWG のサーバーで `setTimestampReset()` を使っていれば、拒否は設定した秒数で解けます。Linux のサーバーでは、そちらの WireGuard を再起動するまで解けません。

## 7. 乱数

鍵の生成と、ハンドシェイクごとの一時鍵に、暗号用の乱数を使います。

| コア | 既定の乱数源 |
|---|---|
| ESP32 | `esp_fill_random()`(ハードウェア乱数) |
| RP2040 / RP2350 | `get_rand_64()` |
| CH32V | TRNG(V307 など)。TRNG のない系列(V208 など)では乱数源がない |
| host | `/dev/urandom` |

乱数源がないと、`begin()` が false を返します。その場合は、`begin()` の前に `wg.setRandom(fn)` で渡してください。`fn` は `bool fn(uint8_t *out, size_t len)` で、暗号用として安全な乱数を返す必要があります。`micros()` や `rand()` は使えません。

## 8. MTU とメモリ

### MTU

- 内側の MTU は、既定で 1420 バイトです。外側のパケットは、内側より 60 バイト大きくなります(IPv4 20 + UDP 8 + WireGuard の頭 16 + 認証タグ 16)
- 外側の経路が 1500 より小さいとき(PPPoE、LTE、別のトンネルの中)は、`setMtu()` で下げてください。範囲は 576 から `TINYWG_MTU` までで、いつでも変えられます
- `TinyWG` では、`wg0` の MTU も同時に変わります。lwIP はそれに合わせて、TCP のセグメントを小さくしたり、UDP を分割したりします
- 送信するパケットが MTU を超えると、`SendResult::Invalid` を返して送りません

### ビルド時の設定

`-D` で上書きできます(`src/tinywg/wg_config.h`)。`arduino-cli` では `--build-property "compiler.cpp.extra_flags=-DTINYWG_MAX_PEERS=8"` のように渡します。

| マクロ | 既定値 | RAM への影響 |
|---|---|---|
| `TINYWG_MAX_PEERS` | 4 | 1 ピアあたり約 0.8 KB |
| `TINYWG_MAX_ALLOWED_IPS` | 4 | 1 項目あたり数バイト |
| `TINYWG_MTU` | 1420 | 送信バッファ、staged packet、`TinyWG` の作業バッファの大きさ(それぞれ約 MTU バイト) |
| `TINYWG_REPLAY_WINDOW` | 128 | 窓の大きさ(ビット)。32 の倍数で 64 以上 |
| `TINYWG_STAGED_PACKET` | 1 | 0 にすると、ハンドシェイク中に送ろうとしたパケットを保持しない(約 MTU バイト減) |

既定の設定で、`Device` は約 6.3 KB です。`TinyWG` 全体では、ESP32 で Flash +25.6 KB、静的 RAM +8.2 KB を使います。詳細は [SPEC.ja.md](../SPEC.ja.md) §5 にあります。

## 9. タイマーと動き

WireGuard のホワイトペーパー §6 の値をそのまま使っています。

| 定数 | 値 | 動き |
|---|---|---|
| REKEY_AFTER_TIME | 120 秒 | セッションを始めた側は、これを過ぎて送信するときに新しいハンドシェイクを始める |
| REJECT_AFTER_TIME | 180 秒 | これより古いセッションでは送受信しない |
| REKEY_TIMEOUT | 5 秒 | ハンドシェイクの返事がなければ再送する |
| REKEY_ATTEMPT_TIME | 90 秒 | 再送をあきらめるまでの時間。次に何か送るときに、またやり直す |
| KEEPALIVE_TIMEOUT | 10 秒 | データを受け取ったのに 10 秒何も送っていなければ、空のパケットを返す(passive keepalive) |
| 鍵のゼロ化 | 540 秒 | 最後のハンドシェイクからこの時間がたつと、セッションの鍵を消す |

同じピアからのハンドシェイクの開始は、1 秒に 50 回までしか受け付けません。それを超えた分は捨てて、`rxFlood` を数えます。

`TINYWG_STAGED_PACKET` が 1 のとき、セッションがない状態で送ろうとした最後の 1 パケットを保持します。ハンドシェイクが済むと送ります(`SendResult::Queued`)。

## 10. 状態の監視と診断

### `peerStatus()`

| 項目 | 意味 |
|---|---|
| `sessionUp` | 今すぐ送れるセッションがある |
| `handshakeInFlight` | 送ったハンドシェイクの返事を待っている |
| `hasHandshake` | 一度でもハンドシェイクが済んだ |
| `lastHandshakeMs` | 最後にハンドシェイクが済んだときの `millis()` |
| `endpoint` | 今の送り先(ローミングで変わる) |
| `rxBytes` / `txBytes` | 外側の UDP のバイト数(WireGuard の頭とタグを含む) |

### `stats()`(デバイス全体の、捨てたパケットの数)

| 項目 | 主な原因 |
|---|---|
| `rxShort` | 短すぎる、または種類のわからないメッセージ |
| `rxBadMac1` | 相手が、こちらの公開鍵を間違えている |
| `rxHandshakeFail` | 復号や認証に失敗したハンドシェイク(PSK 違いなど) |
| `rxUnknownPeer` | 登録していない公開鍵からのハンドシェイク |
| `rxStaleTimestamp` | 時刻が前回より新しくないハンドシェイク(再送攻撃、または相手の時計の巻き戻り) |
| `rxFlood` | 1 秒に 50 回を超えたハンドシェイク |
| `rxNoSession` | 知らないセッション番号宛てのデータ(こちらの再起動の直後などに出る) |
| `rxDecryptFail` | 認証に失敗したデータ(改ざん、または古い鍵) |
| `rxReplay` | 窓の外、またはすでに受け取ったカウンタ |
| `rxNotAllowed` | 送信元が、そのピアの AllowedIPs に入っていない |
| `rxNotIpv4` | 内側が正しい IPv4 ではない |
| `handshakesInitiated` / `Completed` / `Responded` | ハンドシェイクを始めた / 済んだ / 応答した回数 |

## 11. `TinyWGUdp`: 内側のパケットを自分で扱う

lwIP を通さず、内側の IPv4 パケットをスケッチで直接扱います。次の例は、トンネルの中で ping に返事をします。

```cpp
#include <WiFi.h>
#include <WiFiUdp.h>
#include <TinyWG.h>

WiFiUDP udp;
TinyWGUdp wg;

static uint16_t checksum(const uint8_t *p, size_t n)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) sum += ((uint32_t)p[i] << 8) | p[i + 1];
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// packet は書き換えてよい。ここでは ICMP echo request を reply に作り替えて送り返す
static void onPacket(int peer, uint8_t *p, size_t len, void *)
{
    size_t hl = (p[0] & 0x0F) * 4;
    if (p[9] != 1 || p[hl] != 8) return;          // ICMP echo request だけ
    p[hl] = 0;                                    // echo reply
    p[hl + 2] = p[hl + 3] = 0;
    uint16_t c = checksum(p + hl, len - hl);
    p[hl + 2] = c >> 8;
    p[hl + 3] = c;
    uint8_t tmp[4];                               // 送信元と宛先を入れ替える
    memcpy(tmp, p + 12, 4);
    memcpy(p + 12, p + 16, 4);
    memcpy(p + 16, tmp, 4);
    p[10] = p[11] = 0;
    c = checksum(p, hl);
    p[10] = c >> 8;
    p[11] = c;
    wg.send(p, len);
}

void setup()
{
    // ... Wi-Fi に接続
    wg.begin(udp, 51820, "<秘密鍵>");
    int peer = wg.addPeer("<相手の公開鍵>");
    wg.addAllowedIp(peer, IPAddress(10, 9, 0, 1), 32);
    wg.onPacket(onPacket);
}

void loop()
{
    wg.update();   // 受信とタイマー。100 ms より長く空けない
}
```

- `onPacket` の中から `wg.send()` を呼んで構いません
- `send()` は、送り先を AllowedIPs で選びます。トンネル内の自分のアドレスという概念はありません。送信元アドレスは、パケットに書いたものがそのまま使われます

## 12. `tinywg::Device` を直接使う

外側の UDP を自分で用意するときは、`tinywg::Transport` を実装して、`Device` に渡します。

```cpp
#include <TinyWG.h>

class ModemTransport : public tinywg::Transport {
public:
    void sendOuter(const tinywg::Endpoint &to, const uint8_t *data, size_t len) override
    {
        // to.addr[0..3]:to.port に data を UDP で送る
    }
    void deliverInner(int peer, uint8_t *packet, size_t len) override
    {
        // 復号済みの内側の IPv4 パケット。packet は書き換えてよい
    }
};

tinywg::Device dev;
ModemTransport transport;
uint8_t rx[16 + TINYWG_MTU + 32];

void setup()
{
    uint8_t priv[32], pub[32];
    tinywg::keyFromBase64(priv, "<秘密鍵>");
    tinywg::keyFromBase64(pub, "<相手の公開鍵>");
    dev.begin(priv, &transport);
    int p = dev.addPeer(pub);
    const uint8_t net[4] = {10, 9, 0, 0};
    dev.addAllowedIp(p, net, 24);
}

void loop()
{
    // 外側の UDP を 1 つ受け取ったら:
    //   tinywg::Endpoint from = {{a, b, c, d}, port};
    //   dev.receiveOuter(rx, len, from, millis());   // rx はその場で復号される
    // 内側のパケットを送るとき:
    //   dev.sendInner(packet, len, millis());
    dev.tick(millis());   // 100 ms 程度ごとに
}
```

時刻は `millis()` のような 32 ビットのミリ秒を渡します。49.7 日での一周も正しく扱います。

## 13. セキュリティ上の注意

- **Cookie Reply は送りません。** 高負荷時の DoS 対策として相手に Cookie を求める機能は、まだありません(受け取る側は対応しています)。インターネットに直接さらすサーバーでは、ハンドシェイクの計算(ESP32 で 1 回約 73 ms)を大量に送りつけられる可能性があります。ピアあたりの回数制限(1 秒 50 回)と、こちらの公開鍵を知らない相手のハンドシェイクを mac1 の段階で(重い計算の前に)捨てる処理はあります
- **鍵の保管:** 秘密鍵は、Flash や NVS に平文で置かれます(§5)
- **IPv4 のみ:** 内側も外側も IPv4 だけです

## 14. テストと計測

- テストの一覧と動かし方は [tests/README.ja.md](../tests/README.ja.md) にあります
- Linux カーネルの WireGuard との相互接続は、自動テストで確かめています
- 実機での計測(X25519、ハンドシェイク、AEAD の速さ、ping、スループット、長時間試験、Wi-Fi の切断)は [SPEC.ja.md](../SPEC.ja.md) §5.2 にあります
