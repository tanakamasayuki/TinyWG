"""Crypto primitives against independent Python implementations.

The DUT prints its output for fixed inputs; every value is recomputed here with
hashlib (BLAKE2s, HMAC) and `cryptography` (X25519, ChaCha20-Poly1305).
"""

import base64
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers import reference as ref  # noqa: E402

P = ref.pattern
KEY = P(32, 1)


def expected_vectors():
    exp = {}
    for n in (0, 3, 63, 64, 65, 128, 129, 300):
        data = P(n, 2)
        exp[f"blake2s/{n}"] = ref.blake2s(data)
        exp[f"mac32/{n}"] = ref.blake2s(data, key=KEY, digest_size=16)
        exp[f"mac16/{n}"] = ref.blake2s(data, key=KEY[:16], digest_size=16)
    for n in (0, 32):
        for i, t in enumerate(ref.kdf(KEY, P(n, 3), 3), 1):
            exp[f"kdf/{n}/{i}"] = t
    a, b = P(32, 4), P(32, 5)
    exp["x25519/pub_a"] = ref.x25519_public(a)
    exp["x25519/pub_b"] = ref.x25519_public(b)
    exp["x25519/shared"] = ref.x25519(a, ref.x25519_public(b))
    for c in (0, 0x0102030405060708):
        for n in (0, 1, 16, 100, 300):
            exp[f"aead/{c & 0xFF}/{n}"] = ref.aead_encrypt(P(32, 6), c, P(n, 8), P(32, 7))
    return exp


def test_crypto_vectors(dut):
    match = dut.expect(re.compile(rb"(?s)(.*)TEST done (\d+)/(\d+)"), timeout=60)
    text = match.group(1).decode(errors="replace")
    passed, total = int(match.group(2)), int(match.group(3))
    assert passed == total, f"{passed}/{total} EXPECTs passed (see PASS/FAIL lines above)"

    got = {}
    for name, value in re.findall(r"VEC (\S+) ([0-9a-f-]+)", text):
        got[name] = b"" if value == "-" else bytes.fromhex(value)

    mismatches = []
    for name, value in expected_vectors().items():
        if got.get(name) != value:
            mismatches.append(f"{name}: dut={got.get(name, b'').hex()} ref={value.hex()}")
    assert not mismatches, "\n".join(mismatches)

    b64 = re.search(r"B64 (\S+)", text).group(1)
    assert b64 == base64.b64encode(P(32, 13)).decode()

    fails = re.findall(r"FAIL (.+)", text)
    assert not fails, fails
