"""Two engines back to back: handshake, data, replay, timers."""

import re


def test_core_pair(dut):
    match = dut.expect(re.compile(rb"TEST done (\d+)/(\d+)"), timeout=120)
    passed = int(match.group(1))
    total = int(match.group(2))
    assert passed == total, f"{passed}/{total} EXPECTs passed (see PASS/FAIL lines above)"
