"""A user-space WireGuard peer in Python, for talking to a board from a PC.

It needs no privileges (one UDP socket, no TUN): the test builds and parses the
inner IPv4 packets itself. Written from the WireGuard whitepaper, with
hashlib (BLAKE2s) and `cryptography` (X25519, ChaCha20-Poly1305); it is checked
against the Linux kernel in pypeer_selftest/.

One peer, one session at a time: enough for tests, not a general implementation.
"""

import hashlib
import hmac
import os
import socket
import struct
import time

from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

CONSTRUCTION = b"Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s"
IDENTIFIER = b"WireGuard v1 zx2c4 Jason@zx2c4.com"
LABEL_MAC1 = b"mac1----"


def _hash(*parts):
    h = hashlib.blake2s()
    for p in parts:
        h.update(p)
    return h.digest()


def _mac(key, data):
    return hashlib.blake2s(data, key=key, digest_size=16).digest()


def _hmac(key, data):
    return hmac.new(key, data, hashlib.blake2s).digest()


def _kdf(key, data, n):
    prk = _hmac(key, data)
    out, t = [], b""
    for i in range(1, n + 1):
        t = _hmac(prk, t + bytes([i]))
        out.append(t)
    return out


def _aead(key, counter, plain, ad):
    return ChaCha20Poly1305(key).encrypt(b"\0" * 4 + struct.pack("<Q", counter), plain, ad)


def _unaead(key, counter, cipher, ad):
    return ChaCha20Poly1305(key).decrypt(b"\0" * 4 + struct.pack("<Q", counter), cipher, ad)


def _pub(priv):
    return priv.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)


def _dh(priv, pub_bytes):
    return priv.exchange(X25519PublicKey.from_public_bytes(pub_bytes))


def _tai64n():
    now = time.time_ns()
    return struct.pack(">QI", 0x400000000000000A + now // 1_000_000_000, now % 1_000_000_000)


class Session:
    def __init__(self, send_key, recv_key, local_index, remote_index):
        self.send_key, self.recv_key = send_key, recv_key
        self.local_index, self.remote_index = local_index, remote_index
        self.send_counter = 0


class WgPeer:
    def __init__(self, private_key: bytes, peer_public_key: bytes, endpoint, psk: bytes = b"\0" * 32,
                 bind=("0.0.0.0", 0)):
        self.static = X25519PrivateKey.from_private_bytes(private_key)
        self.static_pub = _pub(self.static)
        self.peer_pub = peer_public_key
        self.psk = psk
        self.endpoint = endpoint
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(bind)
        self.session = None
        self.inbox = []          # inner packets received while waiting for something else
        self.keepalives = 0
        self.last_timestamp = b"\0" * 12

    def close(self):
        self.sock.close()

    # ---- handshake, initiator

    def handshake(self, timeout=5.0):
        ck = _hash(CONSTRUCTION)
        h = _hash(_hash(ck, IDENTIFIER), self.peer_pub)
        eph = X25519PrivateKey.generate()
        eph_pub = _pub(eph)
        index = struct.unpack("<I", os.urandom(4))[0]
        msg = struct.pack("<II", 1, index) + eph_pub
        h = _hash(h, eph_pub)
        (ck,) = _kdf(ck, eph_pub, 1)
        ck, k = _kdf(ck, _dh(eph, self.peer_pub), 2)
        enc_static = _aead(k, 0, self.static_pub, h)
        h = _hash(h, enc_static)
        ck, k = _kdf(ck, _dh(self.static, self.peer_pub), 2)
        enc_ts = _aead(k, 0, _tai64n(), h)
        h = _hash(h, enc_ts)
        msg += enc_static + enc_ts
        msg += _mac(_hash(LABEL_MAC1, self.peer_pub), msg) + b"\0" * 16
        self.sock.sendto(msg, self.endpoint)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            data = self._recv(deadline)
            if data is None:
                break
            if len(data) != 92 or data[0] != 2:
                continue
            sender, receiver = struct.unpack_from("<II", data, 4)
            if receiver != index:
                continue
            assert _mac(_hash(LABEL_MAC1, self.static_pub), data[:60]) == data[60:76], "response mac1"
            eph_r = data[12:44]
            h2 = _hash(h, eph_r)
            (ck2,) = _kdf(ck, eph_r, 1)
            (ck2,) = _kdf(ck2, _dh(eph, eph_r), 1)
            (ck2,) = _kdf(ck2, _dh(self.static, eph_r), 1)
            ck2, tau, k = _kdf(ck2, self.psk, 3)
            h2 = _hash(h2, tau)
            _unaead(k, 0, data[44:60], h2)   # raises on a wrong key
            send_key, recv_key = _kdf(ck2, b"", 2)
            self.session = Session(send_key, recv_key, index, sender)
            self.send_ip(b"")   # keepalive: confirms the session to the responder
            return True
        return False

    # ---- handshake, responder (the board initiates)

    def _respond(self, data):
        ck = _hash(CONSTRUCTION)
        h = _hash(_hash(ck, IDENTIFIER), self.static_pub)
        assert _mac(_hash(LABEL_MAC1, self.static_pub), data[:116]) == data[116:132], "initiation mac1"
        sender = struct.unpack_from("<I", data, 4)[0]
        eph_i = data[8:40]
        h = _hash(h, eph_i)
        (ck,) = _kdf(ck, eph_i, 1)
        ck, k = _kdf(ck, _dh(self.static, eph_i), 2)
        peer_static = _unaead(k, 0, data[40:88], h)
        assert peer_static == self.peer_pub, "unknown initiator"
        h = _hash(h, data[40:88])
        ck, k = _kdf(ck, _dh(self.static, self.peer_pub), 2)
        ts = _unaead(k, 0, data[88:116], h)
        assert ts > self.last_timestamp, "stale timestamp"
        self.last_timestamp = ts
        h = _hash(h, data[88:116])

        eph = X25519PrivateKey.generate()
        eph_pub = _pub(eph)
        index = struct.unpack("<I", os.urandom(4))[0]
        msg = struct.pack("<III", 2, index, sender) + eph_pub
        h = _hash(h, eph_pub)
        (ck,) = _kdf(ck, eph_pub, 1)
        (ck,) = _kdf(ck, _dh(eph, eph_i), 1)
        (ck,) = _kdf(ck, _dh(eph, self.peer_pub), 1)
        ck, tau, k = _kdf(ck, self.psk, 3)
        h = _hash(h, tau)
        msg += _aead(k, 0, b"", h)
        msg += _mac(_hash(LABEL_MAC1, self.peer_pub), msg) + b"\0" * 16
        recv_key, send_key = _kdf(ck, b"", 2)
        self.session = Session(send_key, recv_key, index, sender)
        self.sock.sendto(msg, self.endpoint)

    # ---- data

    def send_ip(self, packet: bytes):
        s = self.session
        padded = packet + b"\0" * (-len(packet) % 16)
        msg = struct.pack("<IIQ", 4, s.remote_index, s.send_counter) + _aead(s.send_key, s.send_counter, padded, b"")
        s.send_counter += 1
        self.sock.sendto(msg, self.endpoint)

    def recv_ip(self, timeout=3.0):
        """The next inner IPv4 packet (trimmed to its total length), or None."""
        if self.inbox:
            return self.inbox.pop(0)
        deadline = time.monotonic() + timeout
        while True:
            data = self._recv(deadline)
            if data is None:
                return None
            if data[0] == 1 and len(data) == 148:
                self._respond(data)
                continue
            if data[0] != 4 or not self.session:
                continue
            receiver, counter = struct.unpack_from("<IQ", data, 4)
            if receiver != self.session.local_index:
                continue
            plain = _unaead(self.session.recv_key, counter, data[16:], b"")
            if not plain:
                self.keepalives += 1
                continue
            total = struct.unpack_from(">H", plain, 2)[0]
            return plain[:total]

    def pump(self, seconds):
        """Handles whatever arrives for a while (handshakes, keepalives), keeping inner packets."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            p = self.recv_ip(timeout=end - time.monotonic())
            if p:
                self.inbox.append(p)

    def _recv(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        self.sock.settimeout(remaining)
        try:
            data, _ = self.sock.recvfrom(4096)
            return data
        except socket.timeout:
            return None


# ---- inner packets

def _checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def ipv4(src, dst, proto, payload, ident=0):
    hdr = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, 0, 64, proto, 0,
                      socket.inet_aton(src), socket.inet_aton(dst))
    hdr = hdr[:10] + struct.pack("!H", _checksum(hdr)) + hdr[12:]
    return hdr + payload


def udp(src, dst, sport, dport, data):
    length = 8 + len(data)
    pseudo = socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack("!BBH", 0, 17, length)
    seg = struct.pack("!HHHH", sport, dport, length, 0) + data
    csum = _checksum(pseudo + seg) or 0xFFFF
    return ipv4(src, dst, 17, seg[:6] + struct.pack("!H", csum) + seg[8:])


def icmp_echo(src, dst, ident, seq, data=b"tinywg-ping"):
    body = struct.pack("!BBHHH", 8, 0, 0, ident, seq) + data
    body = body[:2] + struct.pack("!H", _checksum(body)) + body[4:]
    return ipv4(src, dst, 1, body)


def parse_ipv4(packet):
    """(src, dst, proto, payload) of an IPv4 packet."""
    ihl = (packet[0] & 0x0F) * 4
    return (socket.inet_ntoa(packet[12:16]), socket.inet_ntoa(packet[16:20]), packet[9], packet[ihl:])
