# TinyWG 仕様書（ドラフト）

> ステータス: 要件整理段階。数値・最小構成・一部の方式は **実測してから決定** する（本文中 `TBD(実測)` と表記）。

## 1. 目的と範囲

**TinyWG** は Arduino 向けの移植性重視な WireGuard 実装。

- 既存の ESP32 / Pico 向け個別ライブラリと異なり、**特定コアに依存しない WG コア + 薄いポーティング層** で構成する
- イニシエータ(クライアント)だけでなく **レスポンダ(サーバー/受信側)** に対応し、複数ピアを扱う
- TLS が重い / 載らない MCU で、暗号化経路を WireGuard で確保する

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
| ESP32 (arduino-esp32) | Wi-Fi / Ethernet | esp-lwip(OS あり、tcpip スレッド) | NAPT(`IP_NAPT`) 利用可 |
| RP2040 / RP2350 (arduino-pico) | CYW43 Wi-Fi / Ethernet | 本家 lwIP(NO_SYS + async_context) | NAPT なし |
| CH32V (ArduinoCore-CH32RV) | 内蔵 Ethernet / USB NIC | lwIP | 詳細構成は要調査 |
| host (lang-ship:host) | ループバック | なし(テスト用) | WG コアと暗号のロジック検証用 |

最小対象チップは `TBD(実測)`。

## 2. 対象外

- lwIP を持たないコア向けの独自 IP スタック(将来検討。初期スコープ外)
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
[Crypto]              X25519 / ChaCha20-Poly1305 / XChaCha20-Poly1305 / BLAKE2s  ← 差し替え可
[Outer Transport]     外側 UDP。lwIP udp_pcb 直叩き or Arduino UDP&   ← TBD(実測)
[Port Layer]          乱数 / 時刻 / 排他 / タイマー駆動                 ← コアごとに実装
```

### 3.1 層の責務

- **WG Core**: lwIP にも Arduino API にも依存しない純 C/C++。入力は「外側 UDP datagram」「内側 IP パケット」、出力も同様。host 上で単体テスト可能にする
- **Port Layer**: コア差分をここに閉じ込める
  - 暗号用乱数(ESP32: `esp_random`、RP2040: ROSC、CH32: TRNG の有無を要調査、他: ユーザ提供)
  - 時刻(`millis()` 相当の単調時刻 + TAI64N 用の壁時計、§4.4)
  - 排他(esp-lwip は tcpip スレッドとの排他、arduino-pico は async_context lock、NO_SYS 環境は不要)
- **Outer Transport**: 方式は実測で決める
  - lwIP `udp_pcb` 直: コピー削減・pbuf 直接利用。lwIP 前提なら全ターゲットで可能
  - Arduino `UDP&`: 移植性は高いがバッファコピーが増える

## 4. 機能要件

### 4.1 プロトコル(必須)

- Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s ハンドシェイク(Initiation / Response)
- Transport Data / Keepalive / PersistentKeepalive
- PresharedKey(任意)
- 鍵更新タイマー(REKEY_AFTER_TIME / REJECT_AFTER_TIME / REKEY_AFTER_MESSAGES / REKEY_TIMEOUT 等)
- セッションの current / previous / next 切替
- リプレイ防止スライディングウィンドウ(幅はコンパイル時設定、既定値 `TBD(実測)`)
- Cookie Reply: 受信・処理は必須、送信(負荷時応答)は任意機能

### 4.2 レスポンダ / ゲートウェイ

- 固定 ListenPort での待ち受け、複数ピア(上限はコンパイル時定数、既定値 `TBD(実測)`)
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
- 対策候補(Port Layer で選択可能にする):
  - NTP / SNTP による壁時計
  - 不揮発領域に保存したブートカウンタからの合成値
  - ユーザ提供の時刻関数
- レスポンダは受信値の比較のみで、壁時計は不要

## 5. 非機能要件

| 項目 | 方針 |
|---|---|
| メモリ確保 | 動的確保なし、または初期化時のみ。ピア表・セッションは静的確保 |
| 実行モデル | スレッド非前提。`update()` を `loop()` から呼ぶポーリング型を基本とし、OS ありコアでは Port Layer で排他 |
| Flash / RAM 目標 | `TBD(実測)`。ターゲットごとに計測し記録する |
| ハンドシェイク時間 | `TBD(実測)`。特に X25519 の各コアでの所要時間 |
| スループット | `TBD(実測)` |
| 内側 MTU | 設定可能(既定 1420、RAM 削減のため縮小可) |
| 暗号バックエンド | 既定はポータブル C 実装。HW アクセラレーション(ESP32 等)は任意で差し替え |

## 6. テスト方針

PCMFlowUDP のテスト構成(pytest-embedded + Arduino CLI、`TEST done N/M` プロトコル)を踏襲する。

- **host (`lang-ship:host`)**: 暗号のテストベクタ(RFC 7748 / 8439 / 7693)、WG Core 同士のハンドシェイク・データ往復
- **Python ピア**: `hashlib.blake2s` + `cryptography` で実装したテスト用 WG ピアと相互接続
- **実機相互接続**: Linux `wg`(wireguard-tools)、wireguard-go、公式モバイルアプリ
- **フットプリント計測**: 各ターゲットのビルドサイズ・RAM・ハンドシェイク時間を記録(release-toolkit の `pre_version_commit` フックで保存)

## 7. 進め方

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
