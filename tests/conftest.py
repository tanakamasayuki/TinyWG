"""Inside a network namespace (`unshare -rn uv run pytest ...`), brings the
loopback interface up before any DUT starts: the host core's serial link runs
over localhost."""

from helpers.netns import bring_loopback_up


def pytest_configure(config):
    bring_loopback_up()
