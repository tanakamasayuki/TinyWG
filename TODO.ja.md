# TODO / 引き継ぎメモ

最終更新: 2026-10-27。作業環境を移すための引き継ぎです。仕様と計測値は [SPEC.ja.md](SPEC.ja.md)、テストの詳細は [tests/README.ja.md](tests/README.ja.md) にあります。

## 現状

- ブランチは `main` のみ。作業はすべてコミット済みで、**push はまだ**
- プロトコルエンジン、lwIP アダプタ(`TinyWG`)、Arduino UDP アダプタ(`TinyWGUdp`)、サンプル 4 本、pytest の試験環境、リリース用ツールまでできている

### 確認できていること

| 対象 | 状態 |
|---|---|
| host(lang-ship:host 1.7.1) | 全テスト通過。Linux カーネル WireGuard との相互接続 7 シナリオ(両方向、ping、MTU 1420、PSK、PSK 違い、300 パケット)も通過 |
| Python 製 WireGuard ピア(`tests/helpers/wg_peer.py`) | カーネル WireGuard と突き合わせて、イニシエータ / レスポンダの両方で正しいことを確認済み |
| ESP32(D0WD-V3) | `crypto_vectors` / `core_pair` / `bench_crypto` / `board_pair` 通過 |
| ESP32-S3 | 同上 |
| ESP32 ↔ ESP32-S3(`board_pair`) | SoftAP 上で TCP 32 KB をトンネル越しに往復。ハンドシェイク 171 ms、約 376 KiB/s、エラーなし |
| arduino-pico 6.3.1(Pico W / Pico 2 W) | ビルドのみ |
| CH32V307(ArduinoCore-CH32RV 0.4.0) | ビルドのみ |

## 中断中の作業

### `board_responder`(PC → ボードの接続試験)がネットワーク環境の都合で通っていない

- テスト自体はできている。ボードは Wi-Fi に接続し(SSID `CPE-1471`、172.16.0.101)、wg0 = 10.9.0.2 で待つ。PC の Python ピアからハンドシェイクを開始する
- 失敗の原因は TinyWG ではなく、ネットワーク環境と判断している
  1. **AP のクライアント分離と思われる**: Windows の Wi-Fi(172.16.0.100/16)から送信元を指定して ping しても届かない。ARP でボードが解決できない
  2. **WSL2 が NAT モード**: WSL からの通信が Windows の有線側の既定経路に出て、上流から Net Unreachable が返る
- その前に使っていた SSID `WLguest2` では、パスワード違いと見られる切断(理由 15)で接続できなかった
- **新しい環境での手順**: PC とボードを、クライアント分離のない同じネットワークに置く。WSL を使うなら `.wslconfig` に `networkingMode=mirrored` を設定する。そのうえで次を実行する
  ```sh
  cd tests
  uv run --env-file .env pytest board_responder --profile esp32 -s
  ```
- 素の到達性は、ボードの UDP echo(7 番、トンネルの外でも待っている)で先に確かめられる
  ```sh
  python3 -c "import socket;s=socket.socket(2,2);s.settimeout(3);s.sendto(b'x',('<board ip>',7));print(s.recvfrom(9))"
  ```

## 残っている作業

### 実機確認
- [ ] `board_responder` を ESP32 / ESP32-S3 で通す(上記)
- [ ] Pico W / Pico 2 W の実機で、`bench_crypto` と `PicoWClient` を動かす。lwIP のロック(`LWIPMutex`)が実機で問題ないか確かめる
- [ ] CH32V307 の実機で、`bench_crypto` と `Ch32EthernetClient` を動かす。TRNG、`yield()` 駆動の lwIP タイマー、Ethernet の pbuf の扱いを確認する
- [ ] ESP32 の `enableNapt()` を実機で確かめる(ピアから LAN / インターネットへ中継)
- [ ] 長時間の試験: 鍵の更新(120 秒ごと)、keepalive、鍵のゼロ化(540 秒)、Wi-Fi 切断からの復帰

### 機能
- [ ] arduino-pico の FreeRTOS モード対応(今はコンパイルエラーで止めている。lwIP 専用タスクへ処理を投げる形が必要)
- [ ] Cookie Reply の送信(高負荷時の DoS 対策)。今は受信のみ
- [ ] エンドポイントをホスト名で指定(DNS)
- [ ] `wg-quick` 形式の設定テキストの読み込み(フットプリントを見て判断)
- [ ] ESP32 で MQTT ブローカーを動かすゲートウェイのサンプル(SPEC の Phase 4)
- [ ] TAI64N の時刻問題: RTC も NTP もない場合の手段(不揮発領域のブートカウンタなど)

### 最適化の候補(実測を見て判断。SPEC §5.1)
- [ ] 受信した pbuf を `ip_input` にそのまま渡すゼロコピー化(アライメントの確認が必要)
- [ ] lwIP アダプタの作業バッファと `Device` の送信バッファの共用(約 1.5 KB 減)
- [ ] 既定値(ピア数、リプレイ防止の窓、staged packet)を実機の数値をもとに決める

### 公開まわり
- [ ] GitHub への push、リリースワークフロー(`.github/workflows/release.yml`)の確認
- [ ] `library.json`(PlatformIO)。必要なら
- [ ] `SPEC.md`(英語版)。今は `SPEC.ja.md` のみ
- [ ] サンプルごとの README

## 環境メモ

- **ツール**: `arduino-cli` 1.5.1、`uv`。テストの依存は `tests/pyproject.toml`(pytest-embedded、cryptography、pyroute2)にあり、`cd tests && uv sync` で入る
- **コア**: 各 `sketch.yaml` のプロファイルで固定している。初回のビルド時に自動でダウンロードされる
  - esp32:esp32 3.3.12
  - lang-ship:host 1.7.1
  - ch32-riscv-ug:ch32rv 0.4.0(ツールチェーンが約 400 MB)
  - rp2040:rp2040 6.3.1
- **`tests/.env`**: git 管理外。`tests/.env.example` をコピーして、シリアルポートと Wi-Fi を書く。Wi-Fi は `build_config.toml` 経由で `WIFI_SSID` / `WIFI_PASSWORD` としてスケッチに渡る
- **カーネル相互接続**: `unshare -rn uv run pytest kernel_interop pypeer_selftest` で動かす。root は不要。カーネルに WireGuard モジュール(`CONFIG_WIREGUARD`)が必要
- **host の DUT の制約**: Serial の接続は 1 プロセス 1 回だけ。そのため、host 向けのテストは 1 モジュール 1 テスト関数にしている(`kernel_interop` はシナリオを内部でループする)
- **ESP32 の loopTask のスタック**: 8 KB。テストスケッチで大きなバッファをスタックに置かないこと(`core_pair` で一度落ちた)
- **CH32 の参照先**: `~/dev_wch/ArduinoCore-CH32RV`。lwIP の設定は `libraries/Network/src/lwipopts.h`、TRNG は `cores/arduino/ch32rv_rng.h`
- **2 台構成の実行例**(ポートは環境に合わせる)
  ```sh
  uv run --env-file .env pytest board_pair --profile esp32 --peer-profile client:esp32s3 \
      --peer-port client:/dev/ttyUSB1 -s
  ```
