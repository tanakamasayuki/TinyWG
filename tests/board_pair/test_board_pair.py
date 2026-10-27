"""Two boards through TinyWG: TCP over lwIP on both ends of the tunnel.

The primary board (ESP32) opens an access point and is the WireGuard server;
peer_client (ESP32-S3) joins it and initiates. Keys and the AP password are
generated per run, so no external Wi-Fi is involved.

    uv run pytest board_pair --profile esp32 --peer-profile client:esp32s3 \\
        --peer-port client:/dev/ttyUSB1 -s
"""

import re
import secrets
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from helpers.wg_kernel import keypair  # noqa: E402


def test_board_pair(dut, peers, record_property):
    client = peers["client"]
    dut.expect("SERVER-WAIT-CONFIG", timeout=30)
    client.expect("CLIENT-WAIT-CONFIG", timeout=30)

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
