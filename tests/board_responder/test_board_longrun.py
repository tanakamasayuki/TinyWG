"""The protocol timers on a real board, over about 13 minutes (opt-in).

Same sketch as test_board_responder.py. Checks, against the Python peer:
- the board, as the initiator of its session, rekeys by itself after
  REKEY_AFTER_TIME (120 s) while UDP echoes keep flowing without loss
- persistent keepalive arrives at the set interval when the tunnel is idle
- with no traffic, the keys are zeroed 3 x REJECT_AFTER_TIME (540 s) after
  the last handshake, and a fresh handshake from the PC brings it back

    TEST_LONG=1 uv run --env-file .env pytest board_responder/test_board_longrun.py --profile esp32 -s
"""

import os
import re
import time

import pytest

from test_board_responder import start, stats, udp_echo

pytestmark = pytest.mark.skipif(os.environ.get("TEST_LONG") != "1", reason="long test: set TEST_LONG=1")

KEEPALIVE_S = 15


def test_board_longrun(dut, record_property):
    m = dut.expect(re.compile(rb"(?:NET_OK (\S+)|NET_ERROR ([^\r\n]+))\r?\n"), timeout=150)
    assert m.group(1), f"network: {m.group(2).decode()}"
    dut.expect("DUT-WAIT-CONFIG", timeout=10)

    peer = start(dut)
    try:
        # The board must be the initiator of the session: only the initiator rekeys on time.
        assert peer.handshake(timeout=5), "no handshake response"
        assert udp_echo(peer, b"first") == b"first"
        dut.write("CONNECT\n")
        dut.expect("CONNECT 1", timeout=5)
        peer.pump(2.0)
        assert peer.responded == 1, "board-initiated handshake not seen"
        t0 = time.monotonic()

        # 1. Traffic every 5 s for 150 s: one rekey from the board at about 120 s.
        lost, rekey_at = 0, None
        while time.monotonic() - t0 < 150:
            if udp_echo(peer, b"tick %.0f" % (time.monotonic() - t0)) is None:
                lost += 1
            peer.pump(5.0 - (time.monotonic() - t0) % 5.0)
            if peer.responded > 1 and rekey_at is None:
                rekey_at = round(peer.last_handshake - t0)
        stats(dut)
        print(f"  rekey by the board at {rekey_at} s, echoes lost {lost}")
        record_property("rekey_s", f"{rekey_at}")
        assert rekey_at is not None and 115 <= rekey_at <= 140, f"rekey at {rekey_at}"
        assert lost == 0

        # 2. Idle with persistent keepalive.
        dut.write(f"KEEPALIVE {KEEPALIVE_S}\n")
        dut.expect("KEEPALIVE 1", timeout=5)
        k0 = peer.keepalives
        peer.pump(4 * KEEPALIVE_S + 5)
        got = peer.keepalives - k0
        print(f"  {got} keepalives in {4 * KEEPALIVE_S + 5} s (interval {KEEPALIVE_S} s)")
        assert 3 <= got <= 5, f"keepalives {got}"
        assert udp_echo(peer, b"after-idle") == b"after-idle"

        # 3. Quiet: the keys go away 540 s after the last handshake. The board may
        # still start one (it sent the last echo and heard nothing back), so the
        # clock runs from whatever handshake comes last.
        dut.write("KEEPALIVE 0\n")
        dut.expect("KEEPALIVE 1", timeout=5)
        zeroed_after = None
        while time.monotonic() - peer.last_handshake < 620:
            peer.pump(20.0)
            if stats(dut)["up"] == "0":
                zeroed_after = time.monotonic() - peer.last_handshake
                break
        print(f"  keys zeroed {zeroed_after} s after the last handshake")
        record_property("zeroed_s", f"{zeroed_after}")
        assert zeroed_after is not None and 520 <= zeroed_after <= 580, f"zeroed after {zeroed_after}"

        # 4. Back again from the PC.
        assert peer.handshake(timeout=5), "no handshake after zeroing"
        assert udp_echo(peer, b"again") == b"again"
        s = stats(dut)
        assert s["replay"] == "0" and s["badmac"] == "0" and s["up"] == "1"
    finally:
        peer.close()
