# テスト

PCMFlowUDP と同じく、[pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) + Arduino CLI で動かします。既定のプロファイルは `lang-ship:host`(PC 上で動くスケッチ)です。

```sh
cd tests
uv run pytest smoke crypto_vectors core_pair time_server bench_crypto
unshare -rn uv run pytest kernel_interop      # Linux カーネル WireGuard との相互接続
```

## 各テスト

| ディレクトリ | 内容 | プロファイル |
|---|---|---|
| `smoke/` | ビルドとハーネスの確認 | host / esp32 / ch32v307 / picow |
| `crypto_vectors/` | BLAKE2s、MAC、KDF、X25519、ChaCha20-Poly1305 の出力を、Python の `hashlib` / `cryptography` で独立に計算した値と照合。AEAD の改ざん検出、低位数点の拒否、Base64 | 同上 |
| `core_pair/` | エンジン 2 つを同じプロセスで向かい合わせる。ハンドシェイク、双方向のデータ、リプレイと改ざんの拒否、AllowedIPs、順序の入れ替わり、keepalive、鍵の更新とゼロ化、再送、PSK 違い、Cookie Reply の受信と mac2、`CookieGuard`(負荷時の cookie、送信元に結びついた mac2)、再起動して時刻が戻ったピアと `setTimestampReset()` | 同上 |
| `time_server/` | `TinyWGTimeServer` の応答(モード、バージョン、stratum、元の送信時刻、配る時刻)、無視すべき要求、chrony があれば実際の NTP クライアントからの問い合わせ | host のみ |
| `kernel_interop/` | Linux カーネルの WireGuard と相互接続。カーネルからの開始(TinyWG がレスポンダ)、TinyWG からの開始(イニシエータ)、ping、MTU 1420 ちょうど、PSK、PSK 違い、300 パケットの連続送信、`CookieGuard` の cookie をカーネルが受け取って再送 | host のみ |
| `bench_crypto/` | Phase 0 の計測。X25519、ハンドシェイクの各段、AEAD / BLAKE2s の速さ、エンジンの RAM | host / esp32 / esp32s3 / ch32v307 / picow |
| `pypeer_selftest/` | 実機テストで PC 側に使う Python 製 WireGuard ピア(`helpers/wg_peer.py`)を、カーネル WireGuard と突き合わせて検証(イニシエータ / レスポンダ、PSK あり) | PC のみ(スケッチなし) |
| `board_responder/` | 実機が Wi-Fi(ESP32)または Ethernet(CH32V307)で TinyWG(lwIP の wg0)を動かし、PC の Python ピアから接続。ping(lwIP が応答)、UDP の echo(1392 B まで)、100 往復、ボードからの再ハンドシェイク、PSK、未登録の鍵の拒否、実行時の MTU 変更(1000 にすると返事が分割される)、ESP32 では NAPT(トンネル越しに LAN のルーターへ ping と DNS) | esp32 / esp32s3(要 `.env` の Wi-Fi)/ ch32v307(要 LAN ケーブル) |
| `board_responder/test_board_longrun.py` | 約 14 分の長時間試験。ボード側からの鍵の更新(120 秒)、persistent keepalive、無通信での鍵のゼロ化(540 秒)と復帰。`TEST_LONG=1` を付けたときだけ動く | esp32 / esp32s3 / ch32v307 |
| `board_responder/test_board_wifi.py` | Wi-Fi の切断と再接続(`WIFIDROP`)。短い切断のあと同じ鍵で復帰、AP から離れている間に始めたハンドシェイクが再接続後の再送で成立、PC 側のポートの変更(NAT の割り当て直し)にボードが追従 | esp32 / esp32s3 |
| `board_pair/` | 実機 2 台。主 DUT が SoftAP を立てて WireGuard サーバー、`peer_client/` が接続してクライアント。トンネル越しの TCP echo(32 KB) | esp32 + esp32s3(外部 AP 不要) |

## kernel_interop の仕組み

カーネルの wg0 を作るには CAP_NET_ADMIN が要ります。`unshare -rn` で新しいユーザー名前空間とネットワーク名前空間を作り、その中で pytest ごと動かします。root は不要で、実際のネットワークには触れません。名前空間の中では `lo` しかないので、`conftest.py` が起動時に `lo` を上げます。権限がない場合、このテストはスキップされます。

- カーネル側: wg0 = 10.9.0.1/24、127.0.0.1:51820(`helpers/wg_kernel.py`、pyroute2 で netlink から設定)
- DUT: 10.9.0.2、127.0.0.1:51821。トンネル内の ICMP echo と UDP echo(7 番)に応答します

host の DUT は Serial の接続を 1 回しか受け付けません。そのため 1 つのテスト関数の中で、シナリオごとに `CFG` コマンドで鍵を変えて設定し直し、シナリオごとの結果を `SCENARIO PASS/FAIL` と junit の property に出します。

## 実機で動かす

`.env.example` を `.env` にコピーし、シリアルポートと Wi-Fi を書きます。Wi-Fi は `build_config.toml` 経由で `WIFI_SSID` / `WIFI_PASSWORD` としてスケッチに渡ります。

```sh
uv run --env-file .env pytest crypto_vectors core_pair bench_crypto --profile esp32 -s
uv run --env-file .env pytest board_responder --profile esp32 -s
uv run --env-file .env pytest board_responder --profile ch32v307 -s
uv run --env-file .env pytest board_pair --profiles=all --peer-profiles=all -s
```

`board_responder` と `board_pair` は、外側の UDP を lwIP にした場合と、Arduino の `UDP` に差し替えた場合(`setOuterUdp()`)で動きます。`board_responder` は 3 通り(lwIP、差し替えてライブラリが回す、差し替えて `update()` で回す)、`board_pair` はサーバーとクライアントで 2×2 の 4 通りです。パラメータごとに、テストの最初に `RESTART` コマンドでボードを再起動します。

`board_pair` は Wi-Fi なので、どのボードの組でも成り立ちます。そのため `--profiles=all --peer-profiles=all` の直積で回します。同じボードが両側に来る組は、プラグインが skip します。サーバーとクライアントの役を入れ替えるので、クライアント側のポートは `TEST_SERIAL_PORT_PEER_CLIENT_<PROFILE>` で profile ごとに書きます(`.env.example` を参照)。

`board_responder` では PC 側から先にハンドシェイクします。PC が NAT の内側(WSL2 など)にあっても、返事は NAT を通って戻ってくるためです。ボードがエンドポイントを覚えた後は、ボードからの再ハンドシェイクも試します。

## リリース前のフルテスト

リリースの前に、ビルドをやり直して(`--clean`)全部を回します。つないだボードはすべて使い、ポートが書かれていないボードの試験は skip になります。

```sh
cd tests
# 1. Linux カーネルの WireGuard との相互接続(ネットワーク名前空間の中)
unshare -rn uv run pytest kernel_interop pypeer_selftest --clean

# 2. host と、つないだすべてのボード。各スケッチの全 profile と、board_pair はボードの全組合せ
uv run --env-file .env pytest --profiles=all --peer-profiles=all --clean
```

- 2 では、`kernel_interop` と `pypeer_selftest` が権限なしで skip されます。1 と分けているのはそのためです。実機の試験は PC の実ネットワークを使うので、名前空間の中では動きません
- 2 には長時間試験の `board_responder/test_board_longrun.py`(1 ボード約 14 分)と、Wi-Fi 切断の試験も含まれます
- skip を合格と取り違えないでください。最後の要約で、skip の理由が「ポート未設定」と「同じボードが両側」だけであることを確かめます(`-rs` で一覧が出ます)
- ほかの人と共有しているボード(この環境の CH32V307 など)は、決められた手順で確保してから回してください

## 実機で計測する

```sh
uv run pytest bench_crypto -s --profile esp32 --port /dev/ttyUSB0
```

値は `BENCH <名前> <値> <単位>` の形で出力され、junit の property にも残ります。
