"""A board running TinyWG (lwIP wg0) on Wi-Fi or Ethernet, reached from the PC.

The PC side is the Python WireGuard peer (helpers/wg_peer.py, itself checked
against the Linux kernel in pypeer_selftest/). The PC initiates, because it may
sit behind NAT (WSL2) where the board cannot reach it first; once the board
knows the PC's endpoint it also initiates a handshake of its own.

    uv run --env-file .env pytest board_responder --profile esp32 -s
    uv run --env-file .env pytest board_responder --profile ch32v307 -s   # Ethernet
    uv run --env-file .env pytest board_responder/test_board_responder.py --profiles=esp32,esp32s3 -s

It runs three times: with the outer side on lwIP's UDP, and on an Arduino UDP
(setOuterUdp, as for a W5500 on the Ethernet library) with wg0 staying in lwIP,
driven by the library itself or by update() in loop().
"""

import base64
import re
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers.wg_kernel import keypair  # noqa: E402
from helpers.wg_peer import WgPeer, icmp_echo, parse_ipv4, udp  # noqa: E402

PC, BOARD = "10.9.0.1", "10.9.0.2"


def start(dut, psk=None):
    """Configures the board with fresh keys; returns the Python peer facing it."""
    board_priv, board_pub = keypair()
    pc_priv, pc_pub = keypair()
    line = f"CFG {board_priv} {pc_pub}" + (f" {psk}" if psk else "")
    dut.write(line + "\n")
    m = dut.expect(re.compile(rb"DUT-READY ip=(\S+) port=(\d+) peer=(\d+)\r?\n"), timeout=20)
    endpoint = (m.group(1).decode(), int(m.group(2)))
    return WgPeer(base64.b64decode(pc_priv), base64.b64decode(board_pub), endpoint,
                  psk=base64.b64decode(psk) if psk else b"\0" * 32)


def udp_echo(peer, payload, sport=40000):
    peer.send_ip(udp(PC, BOARD, sport, 7, payload))
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        p = peer.recv_ip(timeout=deadline - time.monotonic())
        if p is None:
            return None
        src, _, proto, body = parse_ipv4(p)
        if proto == 17 and struct.unpack_from("!H", body, 2)[0] == sport:
            return body[8:]
    return None


def udp_echo_retry(peer, payload):
    """udp_echo with one retry: a single datagram is sometimes lost on Wi-Fi."""
    return udp_echo(peer, payload) or udp_echo(peer, payload)


def stats(dut):
    dut.write("STATS\n")
    m = dut.expect(re.compile(rb"STATS [^\r\n]+(?=\r?\n)"), timeout=5)
    line = m.group(0).decode()
    print("  " + line)
    return dict(kv.split("=") for kv in line.split()[1:])


def dns_query(ident):
    return struct.pack(">HHHHHH", ident, 0x0100, 1, 0, 0, 0) + b"\x07example\x03com\x00" + struct.pack(">HH", 1, 1)


def gateway():
    """The default gateway if it answers DNS from this PC, else None."""
    try:
        out = subprocess.run(["ip", "-4", "route", "show", "default"], capture_output=True, text=True).stdout
        gw = out.split()[2]
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        try:
            s.sendto(dns_query(1), (gw, 53))
            s.recvfrom(512)
        finally:
            s.close()
        return gw
    except (OSError, IndexError):
        return None


def mtu(dut, peer):
    """A smaller MTU set at run time: lwIP splits a reply that no longer fits."""
    dut.write("MTU 1000\n")
    dut.expect(re.compile(rb"MTU 1 1000\r?\n"), timeout=5)
    try:
        assert udp_echo(peer, bytes(972)) == bytes(972)   # 1000-byte IP packet: still whole
        payload = bytes(range(256)) * 5 + bytes(range(112))   # 1392 bytes
        peer.send_ip(udp(PC, BOARD, 40002, 7, payload))
        frags = []
        while len(frags) < 2:
            p = peer.recv_ip()
            assert p is not None, f"reply fragments: got {len(frags)}"
            frags.append(p)
        assert all(len(f) <= 1000 for f in frags), [len(f) for f in frags]
        offsets = sorted(struct.unpack_from("!H", f, 6)[0] & 0x1FFF for f in frags)
        assert offsets[0] == 0 and offsets[1] > 0, offsets
        whole = b"".join(f[20:] for f in sorted(frags, key=lambda f: struct.unpack_from("!H", f, 6)[0] & 0x1FFF))
        assert whole[8:] == payload
        print(f"  MTU 1000: the 1392-byte echo came back in fragments of {[len(f) for f in frags]} bytes")
    finally:
        dut.write("MTU 1420\n")
        dut.expect(re.compile(rb"MTU 1 1420\r?\n"), timeout=5)


def napt(dut, peer, record_property):
    """ESP32 only: the peer reaches the LAN through the board's NAPT.

    The far end is the PC's default gateway, asked for a ping and a DNS query
    (this PC may drop whatever it did not ask for, ping included). Skipped
    when the gateway does not answer DNS from the PC directly.
    """
    gw = gateway()
    if not gw:
        print("  NAPT: no gateway answering DNS, skipped")
        return
    dut.write("NAPT\n")
    m = dut.expect(re.compile(rb"NAPT (\d)\r?\n"), timeout=5)
    if m.group(1) != b"1":
        print("  NAPT: not on this core, skipped")
        return

    peer.send_ip(udp(PC, gw, 40001, 53, dns_query(0x4e41)))
    p = peer.recv_ip()
    assert p is not None, f"no DNS answer from {gw} through NAPT"
    src, dst, proto, body = parse_ipv4(p)
    assert (src, dst, proto) == (gw, PC, 17) and struct.unpack_from("!HH", body) == (53, 40001)
    assert body[8:10] == b"\x4e\x41"

    peer.send_ip(icmp_echo(PC, gw, 0x4e41, 1))
    p = peer.recv_ip()
    assert p is not None, f"no ping reply from {gw} through NAPT"
    src, dst, proto, body = parse_ipv4(p)
    assert (src, dst, proto, body[0]) == (gw, PC, 1, 0)
    print(f"  NAPT: ping and a DNS query to {gw} answered through the board")
    record_property("napt", "ok")


OUTER_MODES = {"lwip": 0, "udp": 1, "udp_loop": 2}


@pytest.mark.parametrize("outer", OUTER_MODES)
def test_board_responder(dut, record_property, outer):
    # The parametrized runs share one flashed board: start it afresh.
    dut.write("RESTART\n")
    dut.expect("RESTART-OK", timeout=30)
    m = dut.expect(re.compile(rb"(?:NET_OK (\S+)|NET_ERROR ([^\r\n]+))\r?\n"), timeout=150)
    assert m.group(1), f"network: {m.group(2).decode()}"
    dut.expect("DUT-WAIT-CONFIG", timeout=10)
    dut.write(f"OUTER {OUTER_MODES[outer]}\n")
    dut.expect("OUTER 1", timeout=5)

    peer = start(dut)
    try:
        # Handshake from the PC; the board's X25519 work dominates the time.
        t = time.monotonic()
        assert peer.handshake(timeout=5), "no handshake response"
        hs_ms = (time.monotonic() - t) * 1000
        print(f"  handshake round trip {hs_ms:.0f} ms")
        record_property("handshake_ms", f"{hs_ms:.0f}")

        # lwIP answers ping on wg0.
        rtts = []
        for seq in range(5):
            t = time.monotonic()
            peer.send_ip(icmp_echo(PC, BOARD, 0x1234, seq))
            src, dst, proto, body = parse_ipv4(peer.recv_ip())
            rtts.append((time.monotonic() - t) * 1000)
            assert (src, dst, proto, body[0]) == (BOARD, PC, 1, 0)
        print(f"  ping rtt {min(rtts):.1f} / {sum(rtts) / len(rtts):.1f} / {max(rtts):.1f} ms")
        record_property("ping_ms_avg", f"{sum(rtts) / len(rtts):.1f}")

        # A WiFiUDP socket on the board echoes through the tunnel, up to the full MTU.
        for size in (1, 100, 512, 1000, 1392):
            payload = bytes((i * 31 + size) & 0xFF for i in range(size))
            assert udp_echo_retry(peer, payload) == payload, f"udp echo {size} bytes"

        # A run of echoes: counters and the replay window over many packets.
        t = time.monotonic()
        ok = sum(udp_echo_retry(peer, b"seq%04d" % i + bytes(200)) is not None for i in range(100))
        rate = 100 * 207 * 2 / (time.monotonic() - t) / 1024
        print(f"  100 sequential echoes: {ok}/100, {rate:.1f} KiB/s both ways")
        record_property("echo_kib_s", f"{rate:.1f}")
        assert ok == 100

        # The board initiates a new handshake to the endpoint it learned.
        old = peer.session.recv_key
        dut.write("CONNECT\n")
        dut.expect("CONNECT 1", timeout=5)
        peer.pump(2.0)
        assert peer.session.recv_key != old, "board-initiated handshake not seen"
        assert udp_echo(peer, b"after-rekey") == b"after-rekey"

        mtu(dut, peer)
        napt(dut, peer, record_property)

        s = stats(dut)
        assert s["replay"] == "0" and s["decrypt"] == "0" and s["badmac"] == "0"
        assert s["up"] == "1"
    finally:
        peer.close()

    # Pre-shared key, then a stranger the board must ignore.
    psk, _ = keypair()
    peer = start(dut, psk=psk)
    try:
        assert peer.handshake(timeout=5)
        assert udp_echo(peer, b"with-psk") == b"with-psk"
    finally:
        peer.close()

    good = start(dut)
    stranger_priv, _ = keypair()
    stranger = WgPeer(base64.b64decode(stranger_priv), good.peer_pub, good.endpoint)
    try:
        assert not stranger.handshake(timeout=2), "board answered an unknown key"
        assert stats(dut)["unknown"] == "1"
    finally:
        stranger.close()
        good.close()
