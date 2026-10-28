// wg-quick style configuration text for TinyWG::beginConfig() and
// TinyWGUdp::beginConfig(). Kept apart so a sketch that never calls them does
// not link the parser.

#include <string.h>

#include "TinyWGLwip.h"
#include "TinyWGUdp.h"

namespace {

constexpr size_t kLineMax = 256;

struct Interface {
    uint8_t key[32];
    bool hasKey = false;
    bool hasAddress = false;
    uint8_t address[4];
    uint8_t cidr = 32;
    uint16_t listenPort = 0;
    uint16_t mtu = 0;
};

struct Peer {
    bool active = false;
    int line = 0;
    char publicKey[48] = "";
    char presharedKey[48] = "";
    uint8_t allowed[TINYWG_MAX_ALLOWED_IPS][5];   // address, then prefix length
    int allowedCount = 0;
    bool hasEndpoint = false;
    uint8_t endpoint[4];
    uint16_t endpointPort = 0;
    uint16_t keepalive = 0;
};

enum class Section { None, Interface, Peer, Other };

bool sameKey(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = '\0';
    return s;
}

// A decimal number up to max; s is consumed up to the first non-digit.
bool number(const char *&s, uint32_t max, uint32_t *out)
{
    if (*s < '0' || *s > '9') return false;
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s++ - '0');
        if (v > max) return false;
    }
    *out = v;
    return true;
}

bool ipv4(const char *&s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        uint32_t v;
        if (!number(s, 255, &v)) return false;
        out[i] = (uint8_t)v;
        if (i < 3 && *s++ != '.') return false;
    }
    return true;
}

// "a.b.c.d[/n]" -> address and prefix (32 when absent). The whole of s must match.
bool cidr(const char *s, uint8_t out[4], uint8_t *prefix)
{
    uint32_t n = 32;
    if (!ipv4(s, out)) return false;
    if (*s == '/' && !number(++s, 32, &n)) return false;
    *prefix = (uint8_t)n;
    return *s == '\0';
}

// Splits a comma-separated value in place; calls fn on each trimmed item.
template <typename Fn>
bool eachItem(char *value, Fn fn)
{
    for (char *item = value; item;) {
        char *comma = strchr(item, ',');
        if (comma) *comma = '\0';
        char *t = trim(item);
        if (*t && !fn(t)) return false;
        item = comma ? comma + 1 : nullptr;
    }
    return true;
}

// Walks the text line by line: fn(section, key, value, line) for each
// "Key = Value" line. false (with *errorLine) on a malformed line or when fn fails.
template <typename Fn>
bool eachSetting(const char *text, int *errorLine, Fn fn)
{
    Section section = Section::None;
    int line = 0;
    for (const char *p = text; p && *p;) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        line++;
        char buf[kLineMax];
        bool ok = len < sizeof(buf);
        if (ok) {
            memcpy(buf, p, len);
            buf[len] = '\0';
            char *hash = strchr(buf, '#');
            if (hash) *hash = '\0';
            char *s = trim(buf);
            if (*s == '[') {
                char *close = strchr(s, ']');
                ok = close != nullptr;
                if (ok) {
                    *close = '\0';
                    section = sameKey(s + 1, "Interface") ? Section::Interface
                              : sameKey(s + 1, "Peer")    ? Section::Peer
                                                          : Section::Other;
                    ok = fn(section, nullptr, nullptr, line);
                }
            } else if (*s) {
                char *eq = strchr(s, '=');
                ok = eq != nullptr;
                if (ok) {
                    *eq = '\0';
                    ok = fn(section, trim(s), trim(eq + 1), line);
                }
            }
        }
        if (!ok) {
            if (errorLine) *errorLine = line;
            return false;
        }
        p = end ? end + 1 : nullptr;
    }
    return fn(Section::None, nullptr, nullptr, line + 1);   // end of text: closes the last section
}

bool readInterface(const char *text, Interface *ifc, int *errorLine)
{
    return eachSetting(text, errorLine, [&](Section section, char *key, char *value, int) {
        if (section != Section::Interface || !key) return true;
        if (sameKey(key, "PrivateKey")) return ifc->hasKey = tinywg::keyFromBase64(ifc->key, value);
        if (sameKey(key, "ListenPort")) {
            uint32_t v;
            const char *s = value;
            if (!number(s, 65535, &v) || *s) return false;
            ifc->listenPort = (uint16_t)v;
            return true;
        }
        if (sameKey(key, "MTU")) {
            uint32_t v;
            const char *s = value;
            if (!number(s, 65535, &v) || *s) return false;
            ifc->mtu = (uint16_t)v;
            return true;
        }
        if (sameKey(key, "Address")) {
            // The first IPv4 address; IPv6 ones are skipped.
            return eachItem(value, [&](char *item) {
                if (strchr(item, ':') || ifc->hasAddress) return true;
                return ifc->hasAddress = cidr(item, ifc->address, &ifc->cidr);
            });
        }
        return true;   // DNS, Table, PostUp and the like: not for a microcontroller
    });
}

bool addPeer(TinyWGBase &wg, const Peer &p)
{
    int index = wg.addPeer(p.publicKey, p.presharedKey[0] ? p.presharedKey : nullptr);
    if (index < 0) return false;
    for (int i = 0; i < p.allowedCount; i++) {
        const uint8_t *a = p.allowed[i];
        if (!wg.addAllowedIp(index, IPAddress(a[0], a[1], a[2], a[3]), a[4])) return false;
    }
    if (p.hasEndpoint) {
        const uint8_t *e = p.endpoint;
        wg.setEndpoint(index, IPAddress(e[0], e[1], e[2], e[3]), p.endpointPort);
    }
    if (p.keepalive) wg.setPersistentKeepalive(index, p.keepalive);
    return true;
}

bool readPeers(TinyWGBase &wg, const char *text, int *errorLine)
{
    Peer peer;
    int failedLine = 0;
    bool ok = eachSetting(text, errorLine, [&](Section section, char *key, char *value, int line) {
        if (!key) {
            // A section starts, or the text ends: the peer collected so far is complete.
            if (peer.active && !addPeer(wg, peer)) {
                failedLine = peer.line;
                return false;
            }
            peer = Peer();
            peer.active = section == Section::Peer;
            peer.line = line;
            return true;
        }
        if (section != Section::Peer) return true;
        if (sameKey(key, "PublicKey")) return strlen(value) < sizeof(peer.publicKey) && strcpy(peer.publicKey, value);
        if (sameKey(key, "PresharedKey"))
            return strlen(value) < sizeof(peer.presharedKey) && strcpy(peer.presharedKey, value);
        if (sameKey(key, "AllowedIPs")) {
            return eachItem(value, [&](char *item) {
                if (strchr(item, ':')) return true;   // IPv6: not carried
                if (peer.allowedCount >= TINYWG_MAX_ALLOWED_IPS) return false;
                uint8_t *a = peer.allowed[peer.allowedCount++];
                return cidr(item, a, &a[4]);
            });
        }
        if (sameKey(key, "Endpoint")) {
            // An IPv4 address and port; a host name is the sketch's to resolve.
            const char *s = value;
            uint32_t port;
            if (!ipv4(s, peer.endpoint) || *s++ != ':' || !number(s, 65535, &port) || *s) return false;
            peer.endpointPort = (uint16_t)port;
            return peer.hasEndpoint = true;
        }
        if (sameKey(key, "PersistentKeepalive")) {
            if (sameKey(value, "off")) return true;
            uint32_t v;
            const char *s = value;
            if (!number(s, 65535, &v) || *s) return false;
            peer.keepalive = (uint16_t)v;
            return true;
        }
        return true;
    });
    if (!ok && failedLine && errorLine) *errorLine = failedLine;
    return ok;
}

}  // namespace

#ifdef TINYWG_HAS_LWIP
bool TinyWG::beginConfig(const char *text, int *errorLine)
{
    if (errorLine) *errorLine = 0;
    Interface ifc;
    if (!readInterface(text, &ifc, errorLine) || !ifc.hasKey || !ifc.hasAddress) return false;
    if (ifc.mtu && !setMtu(ifc.mtu)) return false;
    uint32_t mask = ifc.cidr ? 0xFFFFFFFFu << (32 - ifc.cidr) : 0;
    const uint8_t *a = ifc.address;
    bool ok = begin(IPAddress(a[0], a[1], a[2], a[3]),
                    IPAddress((uint8_t)(mask >> 24), (uint8_t)(mask >> 16), (uint8_t)(mask >> 8), (uint8_t)mask), ifc.key,
                    ifc.listenPort);
    memset(ifc.key, 0, sizeof(ifc.key));
    if (ok && !readPeers(*this, text, errorLine)) {
        end();
        ok = false;
    }
    return ok;
}
#endif

bool TinyWGUdp::beginConfig(UDP &udp, const char *text, int *errorLine)
{
    if (errorLine) *errorLine = 0;
    Interface ifc;
    if (!readInterface(text, &ifc, errorLine) || !ifc.hasKey) return false;
    if (ifc.mtu && !setMtu(ifc.mtu)) return false;
    bool ok = begin(udp, ifc.listenPort, ifc.key);
    memset(ifc.key, 0, sizeof(ifc.key));
    if (ok && !readPeers(*this, text, errorLine)) {
        end();
        ok = false;
    }
    return ok;
}
