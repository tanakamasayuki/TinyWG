"""Network-namespace helpers for the kernel interop tests.

The interop tests need CAP_NET_ADMIN to create a WireGuard interface. Without
root, run pytest in a fresh user + network namespace:

    unshare -rn uv run pytest kernel_interop

Everything (the kernel wg0, the host-core DUT and pytest) then lives in that
namespace, isolated from the machine's real network.
"""

from pyroute2 import IPRoute


def bring_loopback_up():
    """Bring `lo` up when we may (inside our own namespace); otherwise no-op."""
    try:
        with IPRoute() as ip:
            idx = ip.link_lookup(ifname="lo")[0]
            ip.link("set", index=idx, state="up")
    except Exception:
        pass


def can_create_wireguard():
    """True when a WireGuard interface can be created here."""
    try:
        with IPRoute() as ip:
            ip.link("add", ifname="wgprobe", kind="wireguard")
            ip.link("del", index=ip.link_lookup(ifname="wgprobe")[0])
        return True
    except Exception:
        return False
