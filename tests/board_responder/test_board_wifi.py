"""The tunnel across Wi-Fi outages and a changed endpoint (ESP32).

Same sketch as test_board_responder.py; WIFIDROP takes the board off the AP
for a while and joins again. Checks, against the Python peer:
- after a short drop the session carries on with the same keys, and the
  board's persistent keepalive tells the PC where it is (also if DHCP gave it
  a new address)
- a handshake the board starts while off the AP goes out by retransmission
  once it is back, and the tunnel works on the new keys
- when the PC's port changes (a NAT rebinding), the board follows it

    uv run --env-file .env pytest board_responder/test_board_wifi.py --profile esp32 -s
"""

import re
import time

import pytest

from test_board_responder import start, stats, udp_echo

KEEPALIVE_S = 5


def drop(dut, ms):
    dut.write(f"WIFIDROP {ms}\n")
    m = dut.expect(re.compile(rb"WIFIDROP (down|0 -)\r?\n"), timeout=5)
    if m.group(1) != b"down":
        pytest.skip("no Wi-Fi on this board")


def rejoin(dut, ms):
    """Waits for the board to be back on the AP; returns its address and the time it took."""
    t = time.monotonic()
    m = dut.expect(re.compile(rb"WIFIDROP (\d) (\S+)\r?\n"), timeout=ms / 1000 + 70)
    assert m.group(1) == b"1", "the board did not join the AP again"
    return m.group(2).decode(), time.monotonic() - t


def test_board_wifi(dut, record_property):
    m = dut.expect(re.compile(rb"(?:NET_OK (\S+)|NET_ERROR ([^\r\n]+))\r?\n"), timeout=150)
    assert m.group(1), f"network: {m.group(2).decode()}"
    dut.expect("DUT-WAIT-CONFIG", timeout=10)

    peer = start(dut)
    try:
        assert peer.handshake(timeout=5), "no handshake response"
        assert udp_echo(peer, b"before") == b"before"
        dut.write(f"KEEPALIVE {KEEPALIVE_S}\n")
        dut.expect("KEEPALIVE 1", timeout=5)

        # 1. A short drop: what the PC sends meanwhile is lost; afterwards the
        #    same session carries on, found again through the keepalive.
        keys = peer.session.recv_key
        drop(dut, 5000)
        assert udp_echo(peer, b"lost") is None, "an echo came back while the board was off the AP"
        ip, took = rejoin(dut, 5000)
        print(f"  back on the AP as {ip} after {took:.1f} s")
        k0 = peer.keepalives
        peer.pump(KEEPALIVE_S + 2)
        assert peer.keepalives > k0, "no keepalive after joining again"
        assert peer.endpoint[0] == ip
        assert udp_echo(peer, b"after-drop") == b"after-drop"
        assert peer.session.recv_key == keys, "the session should have survived the drop"

        # 2. The board starts a handshake while off the AP. Each try fails to
        #    leave; the retransmission after joining again gets through.
        responded = peer.responded
        drop(dut, 12000)
        dut.write("CONNECT\n")
        dut.expect("CONNECT 1", timeout=5)
        ip, took = rejoin(dut, 12000)
        t = time.monotonic()
        while peer.responded == responded and time.monotonic() - t < 15:
            peer.pump(0.5)
        assert peer.responded > responded, "no handshake from the board after joining again"
        print(f"  handshake started off the AP completed {time.monotonic() - t:.1f} s after joining")
        assert peer.session.recv_key != keys
        assert udp_echo(peer, b"new-keys") == b"new-keys"

        # 3. The PC's port changes: the board answers to the new one, and its
        #    keepalive follows too.
        old_port = peer.sock.getsockname()[1]
        new_port = peer.rebind()
        assert udp_echo(peer, b"rebound") == b"rebound"
        k0 = peer.keepalives
        peer.pump(KEEPALIVE_S + 2)
        assert peer.keepalives > k0, "no keepalive to the new port"
        print(f"  PC port {old_port} -> {new_port}: the board followed")

        s = stats(dut)
        assert s["replay"] == "0" and s["decrypt"] == "0" and s["badmac"] == "0"
        assert s["up"] == "1"
        record_property("wifi_rejoin", "ok")
    finally:
        peer.close()
