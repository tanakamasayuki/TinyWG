"""Two boards through TinyWG: TCP over lwIP on both ends of the tunnel.

The primary board opens an access point and is the WireGuard server;
peer_client joins it and initiates. Keys and the AP password are generated per
run, so no external Wi-Fi is involved.

Each side runs with the outer side on lwIP's UDP or on an Arduino UDP
(setOuterUdp, as for a W5500 on the Ethernet library): four modes per pair.
Over Wi-Fi every pair of boards works, so run the full product; a pair that
would need the same board twice is skipped:

    uv run --env-file .env pytest board_pair --profiles=all --peer-profiles=all -s
"""

import re
import secrets
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers.wg_kernel import keypair  # noqa: E402


MODES = [("lwip", "lwip"), ("outer", "lwip"), ("lwip", "outer"), ("outer", "outer")]


def restart(board, ready):
    """Starts the board afresh: the parametrized runs share one flashed board."""
    board.write("RESTART\n")
    board.expect("RESTART-OK", timeout=30)
    board.expect(ready, timeout=30)


def set_outer(board, mode):
    board.write(f"OUTER {1 if mode == 'outer' else 0}\n")
    board.expect("OUTER 1", timeout=5)


@pytest.mark.parametrize("server_mode,client_mode", MODES, ids=[f"server_{s}-client_{c}" for s, c in MODES])
def test_board_pair(dut, peers, record_property, server_mode, client_mode):
    client = peers["client"]
    restart(dut, "SERVER-WAIT-CONFIG")
    restart(client, "CLIENT-WAIT-CONFIG")
    set_outer(dut, server_mode)
    set_outer(client, client_mode)

    server_priv, server_pub = keypair()
    client_priv, client_pub = keypair()
    ssid = "TinyWG-" + secrets.token_hex(3)
    password = secrets.token_hex(8)

    dut.write(f"CFG {server_priv} {client_pub} {ssid} {password}\n")
    ap_ip = dut.expect(re.compile(rb"SERVER-READY ap_ip=(\S+)\r?\n"), timeout=20).group(1).decode()

    client.write(f"CFG {client_priv} {server_pub} {ssid} {password} {ap_ip}\n")
    m = client.expect(re.compile(rb"(?:CLIENT-READY handshake_ms=(\d+)|CLIENT-ERROR ([^\r\n]+))\r?\n"), timeout=60)
    assert m.group(1), f"client: {m.group(2).decode()}"
    print(f"  handshake {m.group(1).decode()} ms (client clock, incl. Wi-Fi round trip)")
    record_property("handshake_ms", m.group(1).decode())

    for size in (100, 32768):
        client.write(f"RUN {size}\n")
        m = client.expect(re.compile(rb"(?:TCP-ECHO|TCP-ERROR) [^\r\n]+(?=\r?\n)"), timeout=60)
        line = m.group(0).decode()
        print("  " + line)
        assert line.startswith("TCP-ECHO"), line
        fields = dict(kv.split("=") for kv in line.split()[1:])
        assert fields["received"] == str(size) and fields["match"] == "1", line
        record_property(f"tcp_echo_{size}", line)

    dut.write("STATS\n")
    stats = dut.expect(re.compile(rb"STATS [^\r\n]+(?=\r?\n)"), timeout=5).group(0).decode()
    print("  server " + stats)
    fields = dict(kv.split("=") for kv in stats.split()[1:])
    assert fields["replay"] == "0" and fields["decrypt"] == "0" and fields["up"] == "1", stats
