"""TinyWG::beginConfig(): wg0 from wg-quick text on the board, reached from the PC.

Same sketch as test_board_responder.py; the CONF ... END command carries the text.

    uv run --env-file .env pytest board_responder/test_board_config.py --profiles=esp32
"""

import base64
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from helpers.wg_kernel import keypair  # noqa: E402
from helpers.wg_peer import WgPeer  # noqa: E402
from test_board_responder import stats, udp_echo_retry  # noqa: E402


def test_board_config(dut):
    m = dut.expect(re.compile(rb"(?:NET_OK (\S+)|NET_ERROR ([^\r\n]+))\r?\n"), timeout=150)
    assert m.group(1), f"network: {m.group(2).decode()}"
    dut.expect("DUT-WAIT-CONFIG", timeout=10)

    board_priv, board_pub = keypair()
    pc_priv, pc_pub = keypair()
    text = f"""[Interface]
PrivateKey = {board_priv}
Address = 10.9.0.2/24, fd00::2/64
ListenPort = 51820
MTU = 1280

[Peer]   # the PC
PublicKey = {pc_pub}
AllowedIPs = 10.9.0.1/32, fd00::1/128
"""
    dut.write("CONF\n")
    for line in text.splitlines():
        dut.write(line + "\n")
    dut.write("END\n")
    m = dut.expect(re.compile(rb"DUT-READY ip=(\S+) port=(\d+) peer=0 mtu=(\d+)\r?\n"), timeout=20)
    assert int(m.group(3)) == 1280
    peer = WgPeer(base64.b64decode(pc_priv), base64.b64decode(board_pub), (m.group(1).decode(), int(m.group(2))))
    try:
        assert peer.handshake(timeout=5), "no handshake response"
        for size in (1, 500, 1200):
            payload = bytes((i * 7 + size) & 0xFF for i in range(size))
            assert udp_echo_retry(peer, payload) == payload, f"udp echo {size} bytes"
        assert stats(dut)["up"] == "1"
    finally:
        peer.close()
