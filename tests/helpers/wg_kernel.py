"""The Linux kernel WireGuard as the reference peer (configured over netlink)."""

import base64

from cryptography.hazmat.primitives import serialization as ser
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
from pyroute2 import IPRoute, WireGuard


def keypair():
    """A new (private, public) key pair, both in Base64."""
    k = X25519PrivateKey.generate()
    priv = k.private_bytes(ser.Encoding.Raw, ser.PrivateFormat.Raw, ser.NoEncryption())
    pub = k.public_key().public_bytes(ser.Encoding.Raw, ser.PublicFormat.Raw)
    return base64.b64encode(priv).decode(), base64.b64encode(pub).decode()


class KernelWg:
    """wg0 with one peer. Address 10.9.0.1/24, listening on 127.0.0.1:listen_port."""

    def __init__(self, ifname="wg0", address="10.9.0.1", prefix=24, listen_port=51820):
        self.ifname = ifname
        self.address = address
        self.prefix = prefix
        self.listen_port = listen_port
        self.private_key, self.public_key = keypair()

    def up(self, peer_public_key, peer_allowed="10.9.0.2/32", peer_endpoint_port=None, preshared_key=None):
        self.down()
        with IPRoute() as ip:
            ip.link("add", ifname=self.ifname, kind="wireguard")
            idx = ip.link_lookup(ifname=self.ifname)[0]
            ip.addr("add", index=idx, address=self.address, prefixlen=self.prefix)
            ip.link("set", index=idx, state="up")
        peer = {"public_key": peer_public_key, "allowed_ips": [peer_allowed]}
        if peer_endpoint_port:
            peer.update(endpoint_addr="127.0.0.1", endpoint_port=peer_endpoint_port)
        if preshared_key:
            peer["preshared_key"] = preshared_key
        WireGuard().set(self.ifname, private_key=self.private_key, listen_port=self.listen_port, peer=peer)
        return self

    def down(self):
        with IPRoute() as ip:
            for idx in ip.link_lookup(ifname=self.ifname):
                ip.link("del", index=idx)

    def latest_handshake(self):
        """Unix time of the last completed handshake with the peer (0 = none)."""
        info = WireGuard().info(self.ifname)[0]
        peers = info.get_attr("WGDEVICE_A_PEERS") or []
        for p in peers:
            ts = p.get_attr("WGPEER_A_LAST_HANDSHAKE_TIME")
            if ts:
                return ts.get("tv_sec", 0) if isinstance(ts, dict) else ts["tv_sec"]
        return 0
