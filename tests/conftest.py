"""Shared pytest hooks for TinyWG tests.

Wipes each test's `output/` directory before it runs (as in PCMFlowUDP),
and, when pytest runs inside its own network namespace
(`unshare -rn uv run pytest ...`), brings the loopback interface up before
any DUT starts: the host core's serial link runs over localhost.
"""

import shutil
from pathlib import Path

from helpers.netns import bring_loopback_up


def pytest_configure(config):
    bring_loopback_up()


def pytest_runtest_setup(item):
    output_dir = Path(item.fspath).parent / "output"
    if output_dir.exists():
        shutil.rmtree(output_dir)
