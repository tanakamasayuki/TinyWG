"""Reference implementations of the primitives, from Python's own libraries.

They are independent of the C++ code under test: BLAKE2s from hashlib,
X25519 and ChaCha20-Poly1305 from `cryptography`.
"""

import hashlib
import hmac as _hmac

from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305


def pattern(n, seed):
    """The deterministic test input the sketches use: byte i = (i * 7 + seed) & 0xFF."""
    return bytes((i * 7 + seed) & 0xFF for i in range(n))


def blake2s(data, key=b"", digest_size=32):
    return hashlib.blake2s(data, key=key, digest_size=digest_size).digest()


def hmac_blake2s(key, data):
    return _hmac.new(key, data, hashlib.blake2s).digest()


def kdf(key, data, n):
    """WireGuard's KDFn (whitepaper §5.4): HKDF with HMAC-BLAKE2s."""
    prk = hmac_blake2s(key, data)
    out, t = [], b""
    for i in range(1, n + 1):
        t = hmac_blake2s(prk, t + bytes([i]))
        out.append(t)
    return out


def x25519_public(priv):
    k = X25519PrivateKey.from_private_bytes(priv)
    return k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)


def x25519(priv, pub):
    return X25519PrivateKey.from_private_bytes(priv).exchange(X25519PublicKey.from_public_bytes(pub))


def aead_encrypt(key, counter, plaintext, ad):
    nonce = b"\0\0\0\0" + counter.to_bytes(8, "little")
    return ChaCha20Poly1305(key).encrypt(nonce, plaintext, ad)
