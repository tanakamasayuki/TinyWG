# Changelog / 変更履歴

## Unreleased
- (EN) First implementation: WireGuard protocol engine (initiator and responder, multiple peers), lwIP netif adapter (TinyWG), Arduino UDP adapter (TinyWGUdp).
- (JA) 初期実装: WireGuard プロトコルエンジン(イニシエータ / レスポンダ、複数ピア)、lwIP netif アダプタ(TinyWG)、Arduino UDP アダプタ(TinyWGUdp)。
- (EN) Beginner's and advanced guides (docs/), in English and Japanese.
- (JA) 初心者ガイドと上級者ガイド(docs/、日英)。
- (EN) SPEC.md in English; a README for each example.
- (JA) 英語版の SPEC.md、サンプルごとの README。
- (EN) API reference (docs/api.md), in English and Japanese.
- (JA) API リファレンス(docs/api.ja.md、日英)。
- (EN) setTimestampReset(): a responder can accept clients whose clock went back after a reboot.
- (JA) setTimestampReset(): 再起動で時計が戻ったクライアントをレスポンダが受け付けられる。
- (EN) TinyWGTimeServer: an optional simple NTP server, so clients can take the time from the WireGuard server. Esp32Server uses it.
- (JA) TinyWGTimeServer: WireGuard サーバーから時刻を配れる、必要なら使う簡易 NTP サーバー。Esp32Server で使用。
