# テスト

PCMFlowUDP と同じく、[pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) + Arduino CLI で動かします。既定のプロファイルは `lang-ship:host`(PC 上で動くスケッチ)です。

```sh
cd tests
uv run pytest smoke crypto_vectors core_pair bench_crypto
unshare -rn uv run pytest kernel_interop      # Linux カーネル WireGuard との相互接続
```

## 各テスト

| ディレクトリ | 内容 | プロファイル |
|---|---|---|
| `smoke/` | ビルドとハーネスの確認 | host / esp32 / ch32v307 / picow |
| `crypto_vectors/` | BLAKE2s、MAC、KDF、X25519、ChaCha20-Poly1305 の出力を、Python の `hashlib` / `cryptography` で独立に計算した値と照合。AEAD の改ざん検出、低位数点の拒否、Base64 | 同上 |
| `core_pair/` | エンジン 2 つを同じプロセスで向かい合わせる。ハンドシェイク、双方向のデータ、リプレイと改ざんの拒否、AllowedIPs、順序の入れ替わり、keepalive、鍵の更新とゼロ化、再送、PSK 違い | 同上 |
| `kernel_interop/` | Linux カーネルの WireGuard と相互接続。カーネルからの開始(TinyWG がレスポンダ)、TinyWG からの開始(イニシエータ)、ping、MTU 1420 ちょうど、PSK、PSK 違い、300 パケットの連続送信 | host のみ |
| `bench_crypto/` | Phase 0 の計測。X25519、ハンドシェイクの各段、AEAD / BLAKE2s の速さ、エンジンの RAM | host / esp32 / ch32v307 / picow |

## kernel_interop の仕組み

カーネルの wg0 を作るには CAP_NET_ADMIN が要ります。`unshare -rn` で新しいユーザー名前空間とネットワーク名前空間を作り、その中で pytest ごと動かします。root は不要で、実際のネットワークには触れません。名前空間の中では `lo` しかないので、`conftest.py` が起動時に `lo` を上げます。権限がない場合、このテストはスキップされます。

- カーネル側: wg0 = 10.9.0.1/24、127.0.0.1:51820(`helpers/wg_kernel.py`、pyroute2 で netlink から設定)
- DUT: 10.9.0.2、127.0.0.1:51821。トンネル内の ICMP echo と UDP echo(7 番)に応答します

host の DUT は Serial の接続を 1 回しか受け付けません。そのため 1 つのテスト関数の中で、シナリオごとに `CFG` コマンドで鍵を変えて設定し直し、シナリオごとの結果を `SCENARIO PASS/FAIL` と junit の property に出します。

## 実機で計測する

```sh
uv run pytest bench_crypto -s --profile esp32 --port /dev/ttyUSB0
```

値は `BENCH <名前> <値> <単位>` の形で出力され、junit の property にも残ります。
