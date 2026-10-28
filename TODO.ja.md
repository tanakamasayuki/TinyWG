# TODO / 引き継ぎメモ

最終更新: 2026-10-28。仕様と計測値は [SPEC.ja.md](SPEC.ja.md)、テストの詳細は [tests/README.ja.md](tests/README.ja.md) にあります。

## 現状

- ブランチは `main` のみ。作業はすべてコミット済みで、**push はまだ**
- プロトコルエンジン、lwIP アダプタ(`TinyWG`)、Arduino UDP アダプタ(`TinyWGUdp`)、サンプル 4 本、pytest の試験環境、リリース用ツールまでできている

### 確認できていること

| 対象 | 状態 |
|---|---|
| host(lang-ship:host 1.7.1) | 全テスト通過。Linux カーネル WireGuard との相互接続 7 シナリオ(両方向、ping、MTU 1420、PSK、PSK 違い、300 パケット)も通過 |
| Python 製 WireGuard ピア(`tests/helpers/wg_peer.py`) | カーネル WireGuard と突き合わせて、イニシエータ / レスポンダの両方で正しいことを確認済み |
| ESP32(D0WD-V3) / ESP32-S3 | `crypto_vectors` / `core_pair` / `bench_crypto` / `board_pair` / `board_responder` 通過 |
| ESP32 ↔ ESP32-S3(`board_pair`) | SoftAP 上で TCP 32 KB をトンネル越しに往復。ハンドシェイク 135〜171 ms、約 376〜416 KiB/s、エラーなし |
| MTU の実行時変更(`setMtu()`) | host の `core_pair` で範囲と送信の制限、ESP32 / ESP32-S3 / CH32V307 の `board_responder` で、MTU 1000 にすると lwIP が返事を 1000 バイト以下に分割することを確認 |
| ESP32 の NAPT(`board_responder`) | トンネル越しに LAN のルーターへ ping と DNS が通る(受信 pbuf のリンクヘッダの余白不足を直して通過) |
| ESP32 / CH32V307 の長時間試験(`board_responder/test_board_longrun.py`) | ボード側からの鍵の更新が 120 秒、keepalive が設定どおり、鍵のゼロ化が最後のハンドシェイクから約 540 秒、その後の再接続も通過 |
| ESP32 / ESP32-S3 の Wi-Fi 切断(`board_responder/test_board_wifi.py`) | 5 秒の切断のあと同じ鍵で復帰、切断中に始めたハンドシェイクが再接続後の再送で成立、PC 側のポート変更にボードが追従 |
| PC → ESP32 / ESP32-S3 / CH32V307(`board_responder`) | ESP32 系は家庭用 AP 経由、CH32V307 は Ethernet。ハンドシェイク往復 94 / 100 / 106 ms、ping 約 3 / 3 / 1 ms、UDP echo 100 往復、ボードからの再ハンドシェイク、PSK、未登録の鍵の拒否 |
| arduino-pico 6.3.1(Pico W / Pico 2 W) | ビルドのみ |
| CH32V307(ArduinoCore-CH32RV 0.4.0、96 MHz) | 実機で `smoke` / `crypto_vectors` / `core_pair` / `bench_crypto` / `board_responder`(Ethernet)通過(X25519 24 ms、レスポンダのハンドシェイク処理 103 ms) |

## 残っている作業

### 実機確認
- [ ] Pico W / Pico 2 W の実機で、`bench_crypto` と `PicoWClient` を動かす。lwIP のロック(`LWIPMutex`)が実機で問題ないか確かめる

### 機能
- [ ] arduino-pico の FreeRTOS モード対応(今はコンパイルエラーで止めている。lwIP 専用タスクへ処理を投げる形が必要)
- [ ] Cookie Reply の送信(高負荷時の DoS 対策)。今は受信のみ
- [ ] エンドポイントをホスト名で指定(DNS)
- [ ] `wg-quick` 形式の設定テキストの読み込み(フットプリントを見て判断)
- [ ] ESP32 で MQTT ブローカーを動かすゲートウェイのサンプル(SPEC の Phase 4)
- [ ] 外側を任意の Arduino `UDP` に差し替えるモード(lwIP のあるコア向け)。内側は lwIP の wg0 のまま、外側をモジュール側のスタックで送る。W5500 + `Ethernet` ライブラリ、SIM7080G などの AT コマンドの UDP が対象。受信と送信は `loop()` の `update()` で行い、wg0 からの送信はキューにためる。ループ防止の経路チェックはこのモードでは外す。W5500 の実機は手元にあるが未接続
- [ ] lwIP 経由の W5500(ESP32 の `ETH`、arduino-pico の `W5500lwIP`)での wg0 の実機確認

### 最適化の候補(実測を見て判断。SPEC §5.1)
- [ ] 受信した pbuf を `ip_input` にそのまま渡すゼロコピー化(アライメントの確認が必要)
- [ ] lwIP アダプタの作業バッファと `Device` の送信バッファの共用(約 1.5 KB 減)
- [ ] 既定値(ピア数、リプレイ防止の窓、staged packet)を実機の数値をもとに決める

### 公開まわり
- [ ] GitHub への push、リリースワークフロー(`.github/workflows/release.yml`)の確認
- [ ] `library.json`(PlatformIO)。必要なら

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
- **CH32 の実機を使うとき**: ch32rv のセッション(ArduinoCore-CH32RV)に先に声をかける。使う間は `flock /home/mt/dev_oep/oep-bench/locks/bench.lock <コマンド>` でベンチ共通のロックを取り、始める前と終わった後に、ほかのセッション(dev_oep の "[oep]" など)へ知らせる。ほかのボードの probe や電源には触らない。ESP32 はほかに使う人がいない
- **シリアルポート**: 今の PC では `/run/board-identify/by-id/<チップ>-<ID>` が固定名で使える(`ttyUSB*` / `ttyACM*` の番号は変わる)。`.env` もこの名前で書いている。CH32 は WCH-Link 経由で書き込み、同じポートで Serial が読める
- **実行例**
  ```sh
  cd tests
  uv run --env-file .env pytest board_responder -s                  # 既定は esp32
  uv run --env-file .env pytest board_responder --profile esp32s3 -s
  uv run --env-file .env pytest board_responder --profile ch32v307 -s   # Ethernet
  uv run --env-file .env pytest bench_crypto --profile ch32v307 -s
  uv run --env-file .env pytest board_pair --profile esp32 --peer-profile client:esp32s3 -s
  ```
