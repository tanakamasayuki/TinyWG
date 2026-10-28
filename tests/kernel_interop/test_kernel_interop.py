"""TinyWG against the Linux kernel WireGuard, both as responder and initiator.

Needs CAP_NET_ADMIN for the kernel's wg0. Without root, run the whole session in
its own user + network namespace (nothing touches the real network):

    unshare -rn uv run pytest kernel_interop

The kernel side is wg0 = 10.9.0.1/24 on 127.0.0.1:51820; the DUT is 10.9.0.2 on
127.0.0.1:51821 (helpers/wg_kernel.py, kernel_interop.ino).

The host DUT accepts one serial connection per run, so a single test drives every
scenario, reconfiguring the DUT (fresh keys, fresh kernel wg0) for each one and
reporting each scenario's result.
"""

import re
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers.netns import can_create_wireguard  # noqa: E402
from helpers.wg_kernel import KernelWg, keypair  # noqa: E402

pytestmark = pytest.mark.skipif(
    not can_create_wireguard(),
    reason="needs CAP_NET_ADMIN: run as `unshare -rn uv run pytest kernel_interop`",
)

DUT_IP = "10.9.0.2"
DUT_PORT = 51821
KERNEL_PORT = 51820


def configure(dut, kernel, peer_port, psk=None, dut_psk=None):
    """Gives the DUT and the kernel fresh keys; returns once the DUT is listening."""
    priv, pub = keypair()
    kernel.up(pub, peer_endpoint_port=DUT_PORT if not peer_port else None, preshared_key=psk)
    line = f"CFG {priv} {kernel.public_key} {DUT_PORT} {peer_port}"
    dut_psk = dut_psk or psk
    if dut_psk:
        line += f" {dut_psk}"
    dut.write(line + "\n")
    m = dut.expect(re.compile(rb"DUT-READY peer=(\d+) pub=(\S+)\r?\n"), timeout=10)
    assert m.group(2).decode() == pub


def udp_echo(payload, timeout=3.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(payload, (DUT_IP, 7))
        try:
            data, _ = s.recvfrom(4096)
        except socket.timeout:
            return None
        return data


def kernel_initiates(dut, kernel):
    """The kernel starts the handshake; TinyWG is the responder."""
    configure(dut, kernel, peer_port=0)
    for size in (1, 100, 500, 1000):
        payload = bytes((i * 13) & 0xFF for i in range(size))
        assert udp_echo(payload) == payload, f"echo of {size} bytes"
    assert kernel.latest_handshake() > 0


def ping(dut, kernel):
    configure(dut, kernel, peer_port=0)
    r = subprocess.run(["ping", "-c", "3", "-i", "0.2", "-W", "2", DUT_IP], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr


def full_mtu(dut, kernel):
    """A 1420-byte inner packet (the default MTU on both sides)."""
    configure(dut, kernel, peer_port=0)
    payload = bytes(range(256)) * 5 + bytes(1392 - 1280)
    assert len(payload) + 28 == 1420
    assert udp_echo(payload) == payload


def dut_initiates(dut, kernel):
    """TinyWG starts the handshake; the kernel is the responder."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as srv:
        configure(dut, kernel, peer_port=KERNEL_PORT)
        srv.bind(("10.9.0.1", 9000))
        srv.settimeout(5)
        dut.write("HELLO\n")
        data, addr = srv.recvfrom(2048)
    assert data == b"hello-from-tinywg"
    assert addr == (DUT_IP, 5555)
    # The kernel now sends on the session the DUT initiated.
    assert udp_echo(b"back") == b"back"


def preshared_key(dut, kernel):
    psk, _ = keypair()
    configure(dut, kernel, peer_port=0, psk=psk)
    assert udp_echo(b"with-psk") == b"with-psk"


def wrong_preshared_key(dut, kernel):
    psk, _ = keypair()
    other, _ = keypair()
    configure(dut, kernel, peer_port=0, psk=psk, dut_psk=other)
    assert udp_echo(b"nope", timeout=2) is None
    assert kernel.latest_handshake() == 0


def cookie_reply(dut, kernel):
    """TinyWG under load answers the kernel's initiation with a cookie reply; the
    kernel retries with mac2 (after REKEY_TIMEOUT) and gets a session."""
    configure(dut, kernel, peer_port=0)
    dut.write("GUARD 0\n")   # every handshake counts as under load
    dut.expect("GUARD 0", timeout=5)
    try:
        assert udp_echo(b"behind-a-cookie", timeout=15) == b"behind-a-cookie"
        dut.write("STATS\n")
        m = dut.expect(re.compile(rb"cookies=(\d+) underload=(\d+)"), timeout=5)
        assert int(m.group(1)) >= 1, "no cookie reply sent"
    finally:
        dut.write("GUARD -1\n")
        dut.expect("GUARD -1", timeout=5)


def config_text(dut, kernel):
    """The DUT configured from wg-quick text (TinyWGUdp::beginConfig) talks to the kernel,
    in both directions; IPv6 entries, comments and unknown keys are skipped."""
    priv, pub = keypair()
    kernel.up(pub)   # the kernel has no endpoint for the DUT: the DUT starts
    text = f"""
# made by a phone app, more or less
[Interface]
PrivateKey = {priv}
Address = 10.9.0.2/24, fd00::2/64
ListenPort = {DUT_PORT}
MTU = 1380
DNS = 10.9.0.1

[Peer]
PublicKey = {kernel.public_key}
AllowedIPs = 10.9.0.1/32, fd00::1/128
Endpoint = 127.0.0.1:{KERNEL_PORT}
PersistentKeepalive = 25
"""
    dut.write("CONF\n")
    for line in text.splitlines():
        dut.write(line + "\n")
    dut.write("END\n")
    m = dut.expect(re.compile(rb"DUT-READY peer=0 pub=(\S+) mtu=(\d+) endpoint_port=(\d+)\r?\n"), timeout=10)
    assert m.group(1).decode() == pub
    assert int(m.group(2)) == 1380 and int(m.group(3)) == KERNEL_PORT
    deadline = time.monotonic() + 10   # PersistentKeepalive starts the handshake from the DUT
    while kernel.latest_handshake() == 0 and time.monotonic() < deadline:
        time.sleep(0.2)
    assert kernel.latest_handshake() > 0, "no handshake from the DUT"
    assert udp_echo(b"from-config") == b"from-config"

    # A malformed line is reported by number, and nothing is left running.
    dut.write("CONF\n[Interface]\nPrivateKey = {}\n[Peer]\nPublicKey = {}\nEndpoint = vpn.example.com:51820\nEND\n".format(priv, kernel.public_key))
    m = dut.expect(re.compile(rb"DUT-ERROR config line=(\d+)\r?\n"), timeout=10)
    assert int(m.group(1)) == 5, m.group(0)


def burst(dut, kernel):
    """Many packets in a row: counters, replay window and padding over a long run."""
    configure(dut, kernel, peer_port=0)
    sent = 300
    got = set()
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(3)
        assert udp_echo(b"warm-up") == b"warm-up"

        def receive():
            try:
                while len(got) < sent:
                    data, _ = s.recvfrom(2048)
                    got.add(data[:6])
            except (socket.timeout, OSError):
                pass

        # Read while sending, so the test's own socket buffer is not what overflows.
        reader = threading.Thread(target=receive)
        reader.start()
        for i in range(sent):
            s.sendto(b"n%05d" % i + bytes(i % 50), (DUT_IP, 7))
            if i % 20 == 19:
                time.sleep(0.02)
        reader.join(timeout=10)
    dut.write("STATS\n")
    m = dut.expect(re.compile(rb"STATS .*replay=(\d+) decrypt=(\d+)\s"), timeout=5)
    print(f"  burst: {len(got)}/{sent} echoes, {m.group(0).decode()}")
    assert int(m.group(1)) == 0 and int(m.group(2)) == 0, m.group(0)
    assert len(got) == sent, f"{len(got)}/{sent} echoes"


SCENARIOS = [kernel_initiates, ping, full_mtu, dut_initiates, preshared_key, wrong_preshared_key, burst, cookie_reply,
             config_text]


def test_kernel_interop(dut, record_property):
    dut.expect("DUT-WAIT-CONFIG", timeout=30)
    kernel = KernelWg(listen_port=KERNEL_PORT)
    failures = []
    try:
        for scenario in SCENARIOS:
            try:
                scenario(dut, kernel)
                result = "PASS"
            except Exception as e:  # report every scenario, not just the first failure
                result = "FAIL"
                failures.append(f"{scenario.__name__}: {type(e).__name__}: {e}")
            print(f"SCENARIO {result} {scenario.__name__}")
            record_property(f"scenario.{scenario.__name__}", result)
    finally:
        kernel.down()
    assert not failures, "\n".join(failures)
