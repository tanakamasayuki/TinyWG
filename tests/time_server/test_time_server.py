"""TinyWGTimeServer: SNTP replies, the clock it serves, and requests it ignores."""

import shutil
import socket
import struct
import subprocess
import time

NTP_DELTA = 2208988800


def request(port, packet, timeout=2.0):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(packet, ("127.0.0.1", port))
        try:
            return s.recvfrom(1024)[0]
        except socket.timeout:
            return None


def client_packet(version=4, mode=3, transmit=b"\x12\x34\x56\x78\x9a\xbc\xde\xf0"):
    return bytes([(version << 3) | mode, 0, 6, 0]) + bytes(36) + transmit


def ntp_seconds(b):
    sec, frac = struct.unpack("!II", b)
    return sec - NTP_DELTA + frac / 2**32


def test_time_server(dut):
    dut.expect("TIME-READY", timeout=10)

    reply = request(12300, client_packet())
    assert reply is not None and len(reply) == 48
    assert reply[0] == (4 << 3) | 4, "no leap warning, version 4, server mode"
    assert reply[1] == 10, "stratum"
    assert reply[2] == 6, "poll copied"
    assert reply[12:16] == b"LOCL"
    assert reply[24:32] == b"\x12\x34\x56\x78\x9a\xbc\xde\xf0", "origin = client's transmit"
    for at in (16, 32, 40):
        assert abs(ntp_seconds(reply[at : at + 8]) - 1000000000.5) < 1e-6

    reply = request(12300, client_packet(version=3))
    assert reply is not None and reply[0] == (3 << 3) | 4, "the client's version is echoed"

    assert request(12300, client_packet(mode=4), timeout=0.5) is None, "a server-mode packet is ignored"
    assert request(12300, client_packet()[:40], timeout=0.5) is None, "a short packet is ignored"

    reply = request(12301, client_packet())
    assert reply is not None
    assert abs(ntp_seconds(reply[40:48]) - time.time()) < 2, "the default clock is gettimeofday()"

    # A real NTP client, when chrony is installed: -Q measures without setting the clock.
    chronyd = shutil.which("chronyd")
    if chronyd:
        out = subprocess.run(
            [chronyd, "-Q", "-x", "-f", "/dev/null", "server 127.0.0.1 port 12301 iburst maxsamples 1"],
            capture_output=True, text=True, timeout=30,
        )
        assert "System clock wrong by" in out.stdout + out.stderr, out.stdout + out.stderr
