"""Checks the Python WireGuard peer (helpers/wg_peer.py) against the Linux kernel.

The board tests trust this peer, so it is first proven against the reference
implementation, in both roles. No sketch: runs on the PC only.

    unshare -rn uv run pytest pypeer_selftest
"""

import base64
import socket
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers.netns import can_create_wireguard  # noqa: E402
from helpers.wg_kernel import KernelWg, keypair  # noqa: E402
from helpers.wg_peer import WgPeer, icmp_echo, parse_ipv4, udp  # noqa: E402

pytestmark = pytest.mark.skipif(
    not can_create_wireguard(),
    reason="needs CAP_NET_ADMIN: run as `unshare -rn uv run pytest pypeer_selftest`",
)

ME, KERNEL = "10.9.0.2", "10.9.0.1"


@pytest.fixture
def setup():
    priv, pub = keypair()
    psk, _ = keypair()
    kernel = KernelWg(listen_port=51820)
    kernel.up(pub, peer_endpoint_port=51821, preshared_key=psk)
    peer = WgPeer(base64.b64decode(priv), base64.b64decode(kernel.public_key), ("127.0.0.1", 51820),
                  psk=base64.b64decode(psk), bind=("127.0.0.1", 51821))
    yield kernel, peer
    peer.close()
    kernel.down()


def test_initiator(setup):
    kernel, peer = setup
    assert peer.handshake()
    peer.send_ip(icmp_echo(ME, KERNEL, 7, 1))
    src, dst, proto, payload = parse_ipv4(peer.recv_ip())
    assert (src, dst, proto, payload[0]) == (KERNEL, ME, 1, 0)   # echo reply

    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind((KERNEL, 9000))
        s.settimeout(3)
        peer.send_ip(udp(ME, KERNEL, 5555, 9000, b"from-python"))
        data, addr = s.recvfrom(100)
        assert (data, addr) == (b"from-python", (ME, 5555))
        s.sendto(b"to-python", (ME, 5555))
        src, _, proto, payload = parse_ipv4(peer.recv_ip())
        assert proto == 17 and payload[8:] == b"to-python"


def test_responder(setup):
    kernel, peer = setup
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(b"kernel-first", (ME, 7777))   # makes the kernel initiate
        packet = peer.recv_ip(timeout=5)
    assert packet is not None
    _, _, proto, payload = parse_ipv4(packet)
    assert proto == 17 and payload[8:] == b"kernel-first"
