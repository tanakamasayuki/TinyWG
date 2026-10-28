# TinyWG 仕様書（ドラフト）

[English](SPEC.md)

> ステータス: Phase 0〜3 を実装済み(2026-10-27)。ESP32 / ESP32-S3 / CH32V307 は実機で確認済み、Pico W / Pico 2 W はビルドのみ。最小構成など、まだ決めていない値は `TBD` と表記する。
>
> 使い方は [初心者ガイド](docs/guide-beginner.ja.md) と [上級者ガイド](docs/guide-advanced.ja.md) にある。この文書は設計と計測の記録。

## 1. 目的と範囲

**TinyWG** は Arduino 向けの移植性重視な WireGuard 実装。

- 既存の ESP32 / Pico 向け個別ライブラリと異なり、**特定コアに依存しない WG コア + 薄いポーティング層** で構成する
- イニシエータ(クライアント)だけでなく **レスポンダ(サーバー/受信側)** に対応し、複数ピアを扱う
- 用途は 3 つを同列に扱う: ふつうの WireGuard として既存の WG ネットワークに参加する(サーバーにもなる)、TLS が載る MCU でもより軽い暗号として WG を選ぶ、TLS が重い / 載らない MCU で暗号化経路を確保する

### 1.1 想定ユースケース

| # | 構成 | TinyWG の役割 |
|---|---|---|
| UC1 | 小型 MCU(CH32V 等) → インターネット上の WG サーバー(Linux 等) | イニシエータ、1 ピア |
| UC2 | ESP32 等の IoT ゲートウェイを WG サーバー兼 MQTT ブローカーとして LAN に置き、小型 MCU が WG で接続 | GW: レスポンダ・複数ピア / 端末: イニシエータ |
| UC3 | PC・スマホから MCU 上の WG サーバーへ直接接続してメンテナンス | レスポンダ |

### 1.2 対象プラットフォーム

全ターゲットで **lwIP が利用可能** であることを前提とする。

| コア | NIC | lwIP 形態 | 備考 |
|---|---|---|---|
| ESP32 (arduino-esp32 3.3) | Wi-Fi / Ethernet | esp-lwip。OS あり、tcpip スレッド、`LWIP_TCPIP_CORE_LOCKING=y`、スレッド安全性チェックあり | `IP_FORWARD` / `IP_NAPT` 有効 |
| RP2040 / RP2350 (arduino-pico 6.3) | CYW43 Wi-Fi / Ethernet | 本家 lwIP、`NO_SYS=1`。lwIP の API は `--wrap` で再帰ミューテックス(`LWIPMutex`)に包まれる。FreeRTOS モードでは lwIP 専用タスクに要求を投げる方式 | `IP_FORWARD` 無効、NAPT なし |
| CH32V (ArduinoCore-CH32RV 0.4、Network ライブラリ) | 内蔵 Ethernet / USB NIC(NCM) | lwIP 2.2.1、`NO_SYS=1`、raw API。`loop()` の後と `delay()` の中から駆動。lwIP は Network ライブラリに同梱 | `IP_FORWARD` 無効。TRNG は V303/305/307/317、V407/467、H41x のみ(V208 にはない) |
| host (lang-ship:host) | ループバック | なし(テスト用) | `TinyWGUdp` のみ。WG コアと暗号の検証、Linux カーネル WG との相互接続試験 |

最小対象チップは `TBD(実測)`。

## 2. 対象外

- lwIP を持たないコア(AVR、Arduino SAMD、Uno R4、Mbed 系、nRF52 など)向けのトンネル内 IP スタック。これらのコアでは `TinyWGUdp` で生の IPv4 パケットを扱うところまで
- トンネル内 IPv6 `TBD`(lwIP 側の対応次第で検討)
- NAT 越え(STUN 等)、DNS
- WireGuard 以外の VPN プロトコル

## 3. アーキテクチャ

```
App (MQTT ブローカー/クライアント, CoAP, 任意ソケット)
   │  lwIP 標準 API でそのまま通信
[lwIP]
   │  netif (wg0)  ← TinyWG が提供
[WG Netif Adapter]    lwIP netif の input/output 結線、ルーティング、NAPT 連携
[WG Core]             ハンドシェイク / セッション / タイマー / ピア表 / AllowedIPs   ← コア非依存
[Crypto]              X25519 / ChaCha20-Poly1305 / XChaCha20-Poly1305(Cookie) / BLAKE2s
[Outer Transport]     外側 UDP。lwIP udp_pcb 直叩き(TinyWG)/ Arduino UDP&(TinyWGUdp)
[Port Layer]          乱数 / 時刻 / 排他 / タイマー駆動                 ← コアごとに実装
```

### 3.1 層の責務

- **WG Core**: lwIP にも Arduino API にも依存しない純 C/C++。入力は「外側 UDP datagram」「内側 IP パケット」、出力も同様。host 上で単体テスト可能にする
- **Port Layer**: コア差分をここに閉じ込める
  - 暗号用乱数(ESP32: `esp_fill_random`、RP2040 / RP2350: `get_rand_64`、CH32V: TRNG。TRNG のない系列やその他のコアは `setRandom()` でユーザ提供)
  - 時刻(`millis()` 相当の単調時刻 + TAI64N 用の壁時計、§4.4)
  - 排他(esp-lwip は tcpip スレッドとの排他、arduino-pico は `LWIPMutex`、CH32V は不要。§3.2)
- **Outer Transport**: 両方を実装した
  - `TinyWG`(lwIP アダプタ): lwIP の `udp_pcb` を直接使う。受信は pbuf が連続していればその場で復号する
  - `TinyWGUdp`: Arduino の `UDP&`。内側は生の IPv4 パケットをコールバックで渡す(IP スタックなし)。host テストと、lwIP を持たないコア向け

### 3.2 実行コンテキストと排他(実装済み)

`TinyWG` では、外側 UDP の受信コールバック、netif の出力、100 ms ごとの lwIP タイマー(`sys_timeout`)のすべてが lwIP のコンテキストで動く。`loop()` から呼ぶ処理はない。スケッチから呼ぶ公開 API(ピアの追加など)は、コアごとの方法で lwIP と排他する。

| コア | 排他の方法 |
|---|---|
| ESP32 | `LOCK_TCPIP_CORE()` / `UNLOCK_TCPIP_CORE()`。lwIP のコールバック内から公開 API を呼ぶとデッドロックする |
| arduino-pico | `LWIPMutex`(lwIP のラッパーと同じ再帰ロック)。FreeRTOS モードはコンパイルエラーにしている(未対応) |
| CH32V | 不要(lwIP は `loop()` の後と `delay()` の中でしか動かない) |

wg0 の宛先にエンドポイント自身が含まれる(外側の UDP がトンネルに入る)場合、ループを防ぐため送信しない(`ip4_route()` で判定)。

`setOuterUdp(UDP *, bool pollFromLoop)` で、外側だけを任意の Arduino `UDP`(モジュール側の TCP/IP スタック。W5500 の `Ethernet` ライブラリ、モデムの AT ソケットなど)にできる。wg0 は lwIP のまま。外側の送受信は、ESP32 では専用タスク、CH32V では lwIP のタイマーで回し、`pollFromLoop` のときと arduino-pico では `update()` で回す。エンジンからの送信はキュー(8)にためて、外側を回す処理がモジュールへ渡す。このモードではループ防止の経路チェックをしない。

## 4. 機能要件

### 4.1 プロトコル(必須)

- Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s ハンドシェイク(Initiation / Response)
- Transport Data / Keepalive / PersistentKeepalive(設定した時点で 1 つ送る。Linux と同じ)
- PresharedKey(任意)
- 鍵更新タイマー(REKEY_AFTER_TIME / REJECT_AFTER_TIME / REKEY_AFTER_MESSAGES / REKEY_TIMEOUT 等)
- セッションの current / previous / next 切替
- リプレイ防止スライディングウィンドウ(幅はコンパイル時設定 `TINYWG_REPLAY_WINDOW`、既定 128 ビット)
- Cookie Reply: 受信・処理は実装済み。送信(負荷時応答)は未実装

### 4.2 レスポンダ / ゲートウェイ

- 固定 ListenPort での待ち受け、複数ピア(上限はコンパイル時定数 `TINYWG_MAX_PEERS`、既定 4)
- AllowedIPs によるクリプトキールーティング(送信時の宛先→ピア選択、受信時の送信元検証)
- エンドポイントローミング(最後に認証成功したパケットの送信元へ更新)
- wg0 netif 上で lwIP の通常ソケット(MQTT ブローカー等)が待ち受け可能
- NAPT / フォワーディング: ESP32 では `IP_NAPT` と連携して WG 側端末から LAN/インターネットへ出られるようにする。他コアはルーティングのみ
- ゲートウェイ経由のピア間通信 `TBD`

### 4.3 設定

- API による鍵・ピア設定
- `wg-quick` 形式テキスト(`[Interface]` / `[Peer]`)のパース `TBD`(フットプリント次第)
- 鍵は Base64 で受け付ける

### 4.4 時刻(TAI64N)

- イニシエータは TAI64N タイムスタンプを単調増加させる必要がある。RTC なし MCU は再起動後、相手(Linux 等)に古いタイムスタンプとして拒否される
- 既定は `gettimeofday()`(NTP / SNTP で合わせた壁時計)。`setWallClock()` でユーザ提供の関数に差し替えられる(RTC、不揮発領域のブートカウンタからの合成値など。例は上級者ガイド §6)
- 1 回の起動の中では、壁時計が戻っても送る値を単調増加させる
- レスポンダ側の `setTimestampReset(秒)`:応答もデータも指定秒数届いていないピアは、前回の時刻を忘れて古い時刻でも受け付ける。時計のないクライアントを閉域網で使うため。既定 0(忘れない、標準どおり)。何も届いていない間のピアには、記録したハンドシェイク開始の再送で通信を止める妨害ができるようになる(鍵の漏えい、なりすまし、注入は起きない)
- レスポンダは受信値の比較のみで、壁時計は不要
- 簡易 NTP サーバー `TinyWGTimeServer`(任意):WG サーバーが自分の時計を SNTP で配り、クライアントは WG の接続先と同じアドレスで時刻を合わせられる。使わなければコスト 0。どこに合わせるかの指針は初心者ガイド §8.2

## 5. 非機能要件

| 項目 | 方針 |
|---|---|
| メモリ確保 | 動的確保なし、または初期化時のみ。ピア表・セッションは静的確保 |
| 実行モデル | エンジンはスレッド非前提。`TinyWG` は lwIP のコンテキスト(受信コールバックと 100 ms のタイマー)で動き、公開 API は lwIP と排他する(§3.2)。`TinyWGUdp` は `update()` を `loop()` から呼ぶポーリング型 |
| Flash / RAM | 実測(§5.1)。目標値は実機計測後に決める |
| ハンドシェイク時間 | 実測(§5.2)。`tests/bench_crypto` で各コアで計測 |
| スループット | 実測(§5.2) |
| 内側 MTU | 実行時に `setMtu()` で 576 〜 上限の範囲で変更可(既定 1420)。上限 `TINYWG_MTU` はバッファの大きさを決め、RAM 削減のためビルド時に縮小可 |
| 暗号バックエンド | ポータブル C 実装(Monocypher、BLAKE2s)。HW アクセラレーション(ESP32 等)への差し替えは `TBD` |

### 5.1 フットプリント実測(2026-10-27、RAM は 2026-10-28 の削減後、ビルドサイズ)

同じ機能のスケッチ(ネットワーク接続 + UDP 送信 + 時刻合わせ)に、TinyWG を足した増分。既定設定(ピア 4、MTU 1420、窓 128 ビット)。

| ターゲット | Flash | RAM(静的) | 備考 |
|---|---|---|---|
| ESP32(Esp32Client) | +26.9 KB | +5.4 KB | `TinyWG` オブジェクト 5360 B |
| Pico W(PicoWClient) | +23.0 KB | +5.0 KB | `TinyWG` オブジェクト 5008 B |
| CH32V307(Ch32EthernetClient) | +22.4 KB | +5.0 KB | `TinyWG` オブジェクト 4992 B。全体では Flash 80.4 KB / RAM 42.7 KB(lwIP のプールを含む) |

RAM の内訳(host 64 ビットでの `sizeof`): `Device` 4.5 KB(ピア 1 つ 728 B × 4、送信バッファ 1.45 KB、ハンドシェイクのメッセージ用 148 B)。ハンドシェイク待ちのパケットは送信バッファの中で待つ(`setStagingBuffer()` で専用の領域も渡せる)。lwIP アダプタは、連続していない pbuf を `Device` の送信バッファに直接集めるので、作業バッファを持たない。32 ビットのマイコンではもう少し小さい。2026-10-28 に、アダプタの作業バッファ(1.47 KB)とピアごとの派生鍵(64 B × ピア数)をなくし、ハンドシェイク待ちのパケットを送信バッファに置いて、1 台あたり約 2.9 KB 減らした。ESP32 の Flash の増分 1.2 KB は `setOuterUdp()` の分。

Flash の内訳(CH32V307): X25519(Monocypher)約 7.2 KB、エンジン約 6.5 KB、BLAKE2s 約 1.7 KB、ChaCha20-Poly1305 約 1.8 KB、lwIP アダプタ約 2 KB。

削減の候補(実測を見て判断): ピア数・MTU の縮小、受信 pbuf をそのまま `ip_input` に渡すゼロコピー化、小さく遅い X25519(Flash 約 5 KB 減、X25519 が数倍遅くなる。入れるならビルド時設定ではなくリンクで選ぶ形)。

### 5.2 実機での計測(2026-10-27、`tests/bench_crypto`、Arduino 既定の最適化)

| | ESP32(D0WD-V3、240 MHz) | ESP32-S3(240 MHz) | CH32V307(96 MHz、コアの既定) |
|---|---|---|---|
| X25519 1 回 | 17.6 ms | 12.5 ms | 24.3 ms |
| ハンドシェイク: 開始メッセージ作成(イニシエータ) | 36.3 ms | 26.1 ms | 51.2 ms |
| ハンドシェイク: 受信して応答(レスポンダ) | 72.7 ms | 52.0 ms | 103.2 ms |
| ハンドシェイク: 応答を受けて完了(イニシエータ) | 36.5 ms | 26.1 ms | 52.1 ms |
| ChaCha20-Poly1305(1420 B) | 2.6 MB/s(0.53 ms/パケット) | 3.5 MB/s(0.40 ms/パケット) | 1.6 MB/s(0.86 ms/パケット) |
| BLAKE2s | 4.0 MB/s | 5.3 MB/s | 1.3 MB/s |
| `sizeof(Device)` | 6,280 B | 6,280 B | 6,280 B |

実機 2 台(`tests/board_pair`、ESP32 が SoftAP + サーバー、S3 がクライアント): ハンドシェイク 171 ms(Wi-Fi の往復込み)、トンネル越しの TCP echo 32 KB を往復合計 約 376 KiB/s、エラーなし。

PC → ボード(`tests/board_responder`、家庭用 AP 経由、PC の Python ピアが開始): ハンドシェイク往復 ESP32 94 ms / S3 100 ms、トンネル越しの ping 約 3 ms、200 B の UDP echo 100 往復(往復合計 ESP32 約 61 KiB/s、S3 約 69 KiB/s。1 往復ずつ待つため Wi-Fi の遅延律速)、ボードからの再ハンドシェイク、PSK、未登録の鍵の拒否まで通過。CH32V307 も Ethernet で同じ試験に通過(ハンドシェイク往復 106 ms、ping 約 1 ms、UDP echo 100 往復で往復合計 約 158 KiB/s)。`crypto_vectors` / `core_pair` / `smoke` も実機で通過。`TinyWGTimeServer` は、ESP32-S3 上のサーバーに ESP32 の SNTP クライアント(lwIP)と PC の chrony が同期できることを確認。

長時間試験(`tests/board_responder/test_board_longrun.py`、ESP32、約 14 分): ボード側からの鍵の更新が 120 秒ちょうど(その間の echo の取りこぼし 0)、persistent keepalive 15 秒で 65 秒間に 4 回、無通信で最後のハンドシェイクから 547 秒後に鍵のゼロ化(20 秒ごとの確認なので 540 秒と矛盾しない)、その後 PC からのハンドシェイクで復帰。CH32V307(Ethernet、lwIP のタイマーは `loop()` の後と `delay()` の中で回る)でも同じ結果: 鍵の更新 120 秒(取りこぼし 0)、keepalive 65 秒間に 5 回(設定した時点の 1 回を含む)、鍵のゼロ化 547 秒後、その後の復帰。

Wi-Fi の切断(`tests/board_responder/test_board_wifi.py`、ESP32 / S3): AP から 5 秒離れても同じ鍵のまま復帰(再接続まで約 3 秒、その後 keepalive で PC に居場所を知らせる)。AP から離れている間にボードが始めたハンドシェイクは、送れないまま 5 秒ごとに再送され、再接続から 2.5〜3 秒で成立。PC 側のポートが変わっても(NAT の割り当て直し)ボードが追従。

Pico W の実機計測は未実施。

## 6. テスト方針

PCMFlowUDP のテスト構成(pytest-embedded + Arduino CLI、`TEST done N/M` プロトコル)を踏襲する。

- **host (`lang-ship:host`)**: 暗号のテストベクタ(RFC 7748 / 8439 / 7693)、WG Core 同士のハンドシェイク・データ往復
- **Python ピア**: `hashlib.blake2s` + `cryptography` で実装したテスト用 WG ピアと相互接続
- **実機相互接続**: Linux カーネルの WireGuard とは自動テストで確認済み(`tests/kernel_interop`)。wireguard-go と公式モバイルアプリは未確認
- **フットプリント計測**: 各ターゲットのビルドサイズ・RAM・ハンドシェイク時間を記録する。リリースワークフローに `tools/release_hooks/pre_version_commit.py` を呼ぶ口はあるが、スクリプトは未作成(今は §5 に手で記録)

## 7. 進め方

進捗(2026-10-27):

- Phase 0: 済み。フットプリントと、ESP32 / ESP32-S3 / CH32V307 の実機での速度計測(Pico W は未実施)
- Phase 1: 済み。host 上でペア試験と Linux カーネル WireGuard との相互接続試験(イニシエータ / レスポンダ両方)が通っている
- Phase 2: ESP32 / ESP32-S3 の実機で動作確認済み(単体テスト、計測、ボード 2 台での TCP、PC からボードへの接続試験 `board_responder`)
- Phase 3: CH32V307 は実機で単体テスト、計測、Ethernet 越しの `board_responder` と長時間試験が通過。arduino-pico(RP2040 / RP2350)はビルドのみ
- Phase 4: ESP32 の NAPT 連携(`enableNapt()`)は実機で確認済み(`board_responder` で、トンネル越しに LAN のルーターへ ping と DNS)。MQTT ブローカーの例は未着手

1. **Phase 0 — 実測**: 暗号プリミティブ単体を各ターゲットでビルドし、Flash/RAM・X25519 時間・ChaChaPoly スループットを計測。外側 UDP 方式(`udp_pcb` 直 / Arduino `UDP&`)の比較
2. **Phase 1 — WG Core**: host 上でハンドシェイク・セッション・タイマーを実装し、Python ピアと相互接続
3. **Phase 2 — lwIP netif アダプタ**: ESP32 で wg0 netif、Linux `wg` と相互接続(イニシエータ / レスポンダ両方)
4. **Phase 3 — 移植**: arduino-pico、CH32RV
5. **Phase 4 — ゲートウェイ機能**: 複数ピア、ESP32 NAPT 連携、MQTT ブローカー例
6. Phase 0〜4 の実測結果をもとに最小対象チップ・既定値を確定

## 8. ライセンスと商標

- TinyWG 本体は **BSD 3-Clause**
- 同梱する第三者コード(暗号実装等)は元ライセンスと著作権表示を維持し、README に一覧を記載する。BSD-3 と両立する寛容型ライセンス(CC0 / PD / BSD / MIT)に限る
- プロトコル層はホワイトペーパー等の公開仕様から実装する。**GPL のコード(Linux カーネル版 WireGuard 等)は参照・流用しない**
- "WireGuard" は Jason A. Donenfeld の登録商標。本ライブラリは WireGuard プロジェクトとは無関係であり、プロトコル名は記述的に使用する
