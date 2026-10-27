"""Phase 0 measurements; the values go to the junit report as properties.

    uv run pytest bench_crypto -s                       # host
    uv run pytest bench_crypto -s --profile esp32 ...   # on a board
"""

import re


def test_bench_crypto(dut, record_property):
    match = dut.expect(re.compile(rb"(?s)(.*)BENCH done"), timeout=600)
    results = re.findall(r"BENCH (\S+) (\S+) (\S+)", match.group(1).decode(errors="replace"))
    for name, value, unit in results:
        print(f"{name:16} {value:>12} {unit}")
        record_property(f"bench.{name}", f"{value} {unit}")
    values = {name: float(value) for name, value, _ in results}
    assert values.get("hs_completed", 0) >= 4, "the timed handshakes did not complete"
