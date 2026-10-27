# Monocypher (vendored)

- Upstream: https://monocypher.org/ (https://github.com/LoupVaillant/Monocypher)
- Version: 4.0.2 (`monocypher-4.0.2.tar.gz`)
- License: BSD-2-Clause OR CC0-1.0 (see `LICENCE.md`)

Upstream file hashes (sha256, before modification):

| File | sha256 |
|---|---|
| src/monocypher.c | afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59 |
| src/monocypher.h | f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374 |

Local modifications (mechanical only):

1. Files renamed to `tinywg_monocypher.c` / `tinywg_monocypher.h`, and the
   `#include` in the `.c` updated, so another Monocypher copy on the include
   path cannot be picked up instead.
2. The `crypto_` prefix replaced with `tinywg_mc_` (the same substitution as
   upstream's `change-prefix.sh tinywg_mc`), so the symbols cannot clash with
   another Monocypher or libsodium linked into the same sketch.

TinyWG uses X25519, ChaCha20-Poly1305 (IETF), XChaCha20-Poly1305 and
`crypto_wipe` / `crypto_verify16`. The rest is dropped by `--gc-sections`.
