// The WireGuard protocol, written from the WireGuard whitepaper (Donenfeld,
// "WireGuard: Next Generation Kernel Network Tunnel", sections 5 and 6).
#include "wg_core.h"

#include <string.h>

#include "wg_crypto.h"

namespace tinywg {

using detail::Peer;
using detail::ReplayWindow;
using detail::Session;

namespace {

const char kConstruction[] = "Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s";
const char kIdentifier[] = "WireGuard v1 zx2c4 Jason@zx2c4.com";
const char kLabelMac1[] = "mac1----";
const char kLabelCookie[] = "cookie--";

enum : uint8_t {
    kTypeInitiation = 1,
    kTypeResponse = 2,
    kTypeCookie = 3,
    kTypeData = 4,
};

constexpr size_t kInitiationLen = 148;
constexpr size_t kResponseLen = 92;
constexpr size_t kCookieLen = 64;
constexpr size_t kDataHeaderLen = 16;
constexpr size_t kDataMinLen = kDataHeaderLen + crypto::kTagSize;

// Whitepaper §6.1, in milliseconds.
constexpr uint32_t kRekeyAfterTime = 120000;
constexpr uint32_t kRejectAfterTime = 180000;
constexpr uint32_t kRekeyAttemptTime = 90000;
constexpr uint32_t kRekeyTimeout = 5000;
constexpr uint32_t kKeepaliveTimeout = 10000;
constexpr uint32_t kCookieLifetime = 120000 - 5000;   // the responder's secret age, less latency
constexpr uint32_t kMinInitiationInterval = 1000 / 50; // at most 50 initiations per second per peer
constexpr uint64_t kRekeyAfterMessages = 1ull << 60;
constexpr uint64_t kRejectAfterMessages = UINT64_MAX - (1ull << 13);

void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void setHeader(uint8_t *msg, uint8_t type)
{
    msg[0] = type;
    msg[1] = msg[2] = msg[3] = 0;
}

bool prefixMatch(const uint8_t a[4], const uint8_t b[4], uint8_t cidr)
{
    uint32_t x = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3];
    uint32_t y = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    uint32_t mask = cidr == 0 ? 0 : 0xFFFFFFFFu << (32 - cidr);
    return (x & mask) == (y & mask);
}

void clearSession(Session *s)
{
    crypto::wipe(s, sizeof(*s));
}

bool sessionUsable(const Session &s, uint32_t now)
{
    return s.valid && (now - s.birthMs) < kRejectAfterTime && s.sendCounter < kRejectAfterMessages;
}

// MixHash: h = HASH(h || data)
void mixHash(uint8_t h[32], const uint8_t *data, size_t len) { crypto::hash(h, h, 32, data, len); }

}  // namespace

// ---- replay window

void ReplayWindow::reset()
{
    any = false;
    greatest = 0;
    memset(words, 0, sizeof(words));
}

bool ReplayWindow::acceptable(uint64_t counter) const
{
    if (counter >= kRejectAfterMessages) return false;
    if (!any || counter > greatest) return true;
    if (greatest - counter >= kSpan) return false;
    return !((words[(counter / 32) % kWords] >> (counter % 32)) & 1u);
}

void ReplayWindow::mark(uint64_t counter)
{
    if (!any) {
        memset(words, 0, sizeof(words));
        any = true;
        greatest = counter;
    } else if (counter > greatest) {
        // Clear the words the window slides over (at most all of them).
        uint64_t from = greatest / 32, to = counter / 32;
        for (uint64_t w = from + 1, n = 0; w <= to && n < kWords; w++, n++) words[w % kWords] = 0;
        greatest = counter;
    }
    words[(counter / 32) % kWords] |= 1u << (counter % 32);
}

// ---- device

Device::Device()
    : transport_(nullptr), random_(port::defaultRandom()), wallClock_(port::defaultWallClock), started_(false),
      mtu_(TINYWG_MTU), timestampResetMs_(0), lastTaiSeconds_(0), lastTaiNanos_(0)
{
    memset(peers_, 0, sizeof(peers_));
    memset(&stats_, 0, sizeof(stats_));
#if TINYWG_STAGED_PACKET
    stagedPeer_ = -1;
    stagedLen_ = 0;
#endif
}

bool Device::begin(const uint8_t privateKey[kKeyLen], Transport *transport)
{
    end();
    if (!transport || !random_) return false;
    memcpy(staticPrivate_, privateKey, kKeyLen);
    crypto::clampPrivateKey(staticPrivate_);
    crypto::publicKey(staticPublic_, staticPrivate_);
    crypto::hash(mac1Key_, (const uint8_t *)kLabelMac1, 8, staticPublic_, kKeyLen);
    uint8_t chainKey[32];
    crypto::hash(chainKey, (const uint8_t *)kConstruction, sizeof(kConstruction) - 1);
    crypto::hash(initialHash_, chainKey, 32, (const uint8_t *)kIdentifier, sizeof(kIdentifier) - 1);
    memset(&stats_, 0, sizeof(stats_));
    transport_ = transport;
    started_ = true;
    return true;
}

void Device::end()
{
    crypto::wipe(peers_, sizeof(peers_));
    crypto::wipe(staticPrivate_, sizeof(staticPrivate_));
#if TINYWG_STAGED_PACKET
    stagedPeer_ = -1;
    stagedLen_ = 0;
#endif
    transport_ = nullptr;
    started_ = false;
}

Peer *Device::peer(int index)
{
    if (index < 0 || index >= TINYWG_MAX_PEERS || !peers_[index].used) return nullptr;
    return &peers_[index];
}

const Peer *Device::peer(int index) const
{
    if (index < 0 || index >= TINYWG_MAX_PEERS || !peers_[index].used) return nullptr;
    return &peers_[index];
}

bool Device::randomBytes(uint8_t *out, size_t len) { return random_ && random_(out, len); }

// A fresh local index, unique among this device's sessions and handshakes.
uint32_t Device::newIndex()
{
    for (;;) {
        uint8_t b[4];
        if (!randomBytes(b, 4)) return 0;
        uint32_t idx = get32(b);
        bool used = idx == 0;
        for (int i = 0; i < TINYWG_MAX_PEERS && !used; i++) {
            const Peer &p = peers_[i];
            if (!p.used) continue;
            used = (p.handshake.initiationSent && p.handshake.localIndex == idx) ||
                   (p.current.valid && p.current.localIndex == idx) ||
                   (p.previous.valid && p.previous.localIndex == idx) || (p.next.valid && p.next.localIndex == idx);
        }
        if (!used) return idx;
    }
}

// TAI64N (whitepaper §5.4.2), kept strictly increasing within this boot.
void Device::tai64n(uint8_t out[12])
{
    uint64_t sec;
    uint32_t ns;
    wallClock_(&sec, &ns);
    sec += 0x400000000000000aull;
    if (sec < lastTaiSeconds_ || (sec == lastTaiSeconds_ && ns <= lastTaiNanos_)) {
        sec = lastTaiSeconds_;
        ns = lastTaiNanos_ + 1;
        if (ns >= 1000000000u) {
            sec++;
            ns = 0;
        }
    }
    lastTaiSeconds_ = sec;
    lastTaiNanos_ = ns;
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)(sec >> (56 - 8 * i));
    for (int i = 0; i < 4; i++) out[8 + i] = (uint8_t)(ns >> (24 - 8 * i));
}

// ---- configuration

int Device::addPeer(const uint8_t publicKey[kKeyLen], const uint8_t *presharedKey)
{
    if (!started_ || findPeer(publicKey) >= 0) return -1;
    for (int i = 0; i < TINYWG_MAX_PEERS; i++) {
        Peer &p = peers_[i];
        if (p.used) continue;
        memset(&p, 0, sizeof(p));
        memcpy(p.publicKey, publicKey, kKeyLen);
        if (presharedKey) memcpy(p.presharedKey, presharedKey, kKeyLen);
        if (!crypto::dh(p.staticShared, staticPrivate_, publicKey)) {
            crypto::wipe(&p, sizeof(p));
            return -1;
        }
        crypto::hash(p.mac1Key, (const uint8_t *)kLabelMac1, 8, publicKey, kKeyLen);
        crypto::hash(p.cookieKey, (const uint8_t *)kLabelCookie, 8, publicKey, kKeyLen);
        p.used = true;
        return i;
    }
    return -1;
}

bool Device::removePeer(int index)
{
    Peer *p = peer(index);
    if (!p) return false;
#if TINYWG_STAGED_PACKET
    if (stagedPeer_ == index) stagedPeer_ = -1;
#endif
    crypto::wipe(p, sizeof(*p));
    return true;
}

bool Device::addAllowedIp(int index, const uint8_t addr[4], uint8_t cidr)
{
    Peer *p = peer(index);
    if (!p || cidr > 32 || p->allowedCount >= TINYWG_MAX_ALLOWED_IPS) return false;
    detail::AllowedIp &a = p->allowed[p->allowedCount++];
    memcpy(a.addr, addr, 4);
    a.cidr = cidr;
    return true;
}

bool Device::setEndpoint(int index, const Endpoint &endpoint)
{
    Peer *p = peer(index);
    if (!p) return false;
    p->endpoint = endpoint;
    return true;
}

bool Device::setPersistentKeepalive(int index, uint16_t seconds)
{
    Peer *p = peer(index);
    if (!p) return false;
    p->keepaliveSeconds = seconds;
    // Like Linux, send one right away: an idle tunnel would otherwise never
    // start the timer, which is armed only by traffic.
    p->keepaliveNow = seconds != 0;
    if (seconds == 0) p->persistentKeepalive.disarm();
    return true;
}

bool Device::setMtu(uint16_t mtu)
{
    if (mtu < kMinMtu || mtu > TINYWG_MTU) return false;
    mtu_ = mtu;
    return true;
}

int Device::findPeer(const uint8_t publicKey[kKeyLen]) const
{
    for (int i = 0; i < TINYWG_MAX_PEERS; i++)
        if (peers_[i].used && crypto::equal32(peers_[i].publicKey, publicKey)) return i;
    return -1;
}

int Device::routePeer(const uint8_t addr[4]) const
{
    int best = -1, bestCidr = -1;
    for (int i = 0; i < TINYWG_MAX_PEERS; i++) {
        const Peer &p = peers_[i];
        if (!p.used) continue;
        for (int j = 0; j < p.allowedCount; j++) {
            const detail::AllowedIp &a = p.allowed[j];
            if (a.cidr > bestCidr && prefixMatch(addr, a.addr, a.cidr)) {
                best = i;
                bestCidr = a.cidr;
            }
        }
    }
    return best;
}

bool Device::peerStatus(int index, PeerStatus *out) const
{
    const Peer *p = peer(index);
    if (!p || !out) return false;
    // The status has no clock of its own; "up" here means a session exists that
    // has not yet hit a message limit. Age is checked when sending.
    out->sessionUp = p->current.valid && p->current.sendCounter < kRejectAfterMessages;
    out->handshakeInFlight = p->handshake.initiationSent;
    out->hasHandshake = p->hasHandshake;
    out->lastHandshakeMs = p->lastHandshakeMs;
    out->endpoint = p->endpoint;
    out->rxBytes = p->rxBytes;
    out->txBytes = p->txBytes;
    return true;
}

// ---- timer events (whitepaper §6)

void Device::anySent(Peer *p, uint32_t now)
{
    p->passiveKeepalive.disarm();
    if (p->keepaliveSeconds) p->persistentKeepalive.arm(now + p->keepaliveSeconds * 1000u);
}

void Device::anyReceived(Peer *p, uint32_t now)
{
    p->newHandshake.disarm();
    if (p->keepaliveSeconds) p->persistentKeepalive.arm(now + p->keepaliveSeconds * 1000u);
}

// Only messages that prove the peer holds a live session count: an initiation
// can be a replay, and counting it would keep the timestamp from being forgotten.
void Device::heardFrom(Peer *p, uint32_t now)
{
    p->heardFrom = true;
    p->lastHeardMs = now;
}

bool Device::timestampForgotten(const Peer *p, uint32_t now) const
{
    return timestampResetMs_ && (!p->heardFrom || (now - p->lastHeardMs) >= timestampResetMs_);
}

void Device::sessionDerived(Peer *p, uint32_t now)
{
    p->zeroKeys.arm(now + 3 * kRejectAfterTime);
    p->lastMinuteHandshake = false;
}

void Device::handshakeComplete(Peer *p, uint32_t now)
{
    p->attempting = false;
    p->retransmit.disarm();
    p->hasHandshake = true;
    p->lastHandshakeMs = now;
    stats_.handshakesCompleted++;
}

void Device::wipePeerKeys(Peer *p)
{
    clearSession(&p->current);
    clearSession(&p->previous);
    clearSession(&p->next);
    crypto::wipe(&p->handshake, sizeof(p->handshake));
    p->attempting = false;
    p->retransmit.disarm();
}

void Device::addMacs(Peer *p, uint8_t *msg, size_t len, uint32_t now)
{
    uint8_t *mac1 = msg + len - 32;
    uint8_t *mac2 = msg + len - 16;
    crypto::mac(mac1, p->mac1Key, 32, msg, len - 32);
    memcpy(p->lastMac1, mac1, 16);
    p->hasLastMac1 = true;
    if (p->hasCookie && (now - p->cookieMs) < kCookieLifetime)
        crypto::mac(mac2, p->cookie, 16, msg, len - 16);
    else
        memset(mac2, 0, 16);
}

// ---- handshake: initiator (whitepaper §5.4.2)

bool Device::initiateHandshake(Peer *p, bool retry, uint32_t now)
{
    if (p->endpoint.port == 0) return false;
    if (!retry) {
        if (p->initiationSentOnce && (now - p->lastInitiationTxMs) < kRekeyTimeout) return false;
        if (!p->attempting) {
            p->attempting = true;
            p->attemptStartMs = now;
        }
    }

    detail::Handshake &hs = p->handshake;
    uint8_t *msg = tx_;
    uint8_t key[32], dhOut[32], ephemeralPublic[32];

    hs.initiationSent = false;
    hs.localIndex = newIndex();
    if (!randomBytes(hs.ephemeralPrivate, 32)) return false;
    crypto::clampPrivateKey(hs.ephemeralPrivate);
    crypto::publicKey(ephemeralPublic, hs.ephemeralPrivate);

    crypto::hash(hs.chainKey, (const uint8_t *)kConstruction, sizeof(kConstruction) - 1);
    crypto::hash(hs.hash, initialHash_, 32, p->publicKey, 32);

    setHeader(msg, kTypeInitiation);
    put32(msg + 4, hs.localIndex);
    memcpy(msg + 8, ephemeralPublic, 32);
    mixHash(hs.hash, ephemeralPublic, 32);
    crypto::kdf(hs.chainKey, nullptr, nullptr, hs.chainKey, ephemeralPublic, 32);

    bool ok = crypto::dh(dhOut, hs.ephemeralPrivate, p->publicKey);
    if (ok) {
        crypto::kdf(hs.chainKey, key, nullptr, hs.chainKey, dhOut, 32);
        crypto::aeadEncrypt(msg + 40, key, 0, staticPublic_, 32, hs.hash, 32);
        mixHash(hs.hash, msg + 40, 48);

        crypto::kdf(hs.chainKey, key, nullptr, hs.chainKey, p->staticShared, 32);
        uint8_t timestamp[12];
        tai64n(timestamp);
        crypto::aeadEncrypt(msg + 88, key, 0, timestamp, 12, hs.hash, 32);
        mixHash(hs.hash, msg + 88, 28);

        addMacs(p, msg, kInitiationLen, now);
        hs.initiationSent = true;
    }
    crypto::wipe(key, sizeof(key));
    crypto::wipe(dhOut, sizeof(dhOut));
    if (!ok) {
        crypto::wipe(&hs, sizeof(hs));
        return false;
    }

    transport_->sendOuter(p->endpoint, msg, kInitiationLen);
    stats_.handshakesInitiated++;
    p->initiationSentOnce = true;
    p->lastInitiationTxMs = now;
    uint8_t jitter[2];
    uint32_t extra = randomBytes(jitter, 2) ? (uint32_t)(jitter[0] | (jitter[1] << 8)) % 334 : 0;
    p->retransmit.arm(now + kRekeyTimeout + extra);
    anySent(p, now);
    return true;
}

// ---- handshake: responder (whitepaper §5.4.2, §5.4.3)

void Device::handleInitiation(uint8_t *msg, size_t len, const Endpoint &from, uint32_t now)
{
    if (len != kInitiationLen) {
        stats_.rxShort++;
        return;
    }
    uint8_t expected[16];
    crypto::mac(expected, mac1Key_, 32, msg, kInitiationLen - 32);
    if (!crypto::equal16(expected, msg + 116)) {
        stats_.rxBadMac1++;
        return;
    }

    uint8_t chainKey[32], hash[32], key[32], dhOut[32], peerStatic[32], timestamp[12];
    const uint8_t *ephemeralI = msg + 8;
    Peer *p = nullptr;
    bool ok = false;

    crypto::hash(chainKey, (const uint8_t *)kConstruction, sizeof(kConstruction) - 1);
    crypto::hash(hash, initialHash_, 32, staticPublic_, 32);
    mixHash(hash, ephemeralI, 32);
    crypto::kdf(chainKey, nullptr, nullptr, chainKey, ephemeralI, 32);

    do {
        if (!crypto::dh(dhOut, staticPrivate_, ephemeralI)) break;
        crypto::kdf(chainKey, key, nullptr, chainKey, dhOut, 32);
        if (!crypto::aeadDecrypt(peerStatic, key, 0, msg + 40, 32, hash, 32)) break;
        mixHash(hash, msg + 40, 48);

        int index = findPeer(peerStatic);
        if (index < 0) {
            stats_.rxUnknownPeer++;
            goto out;
        }
        p = &peers_[index];
        crypto::kdf(chainKey, key, nullptr, chainKey, p->staticShared, 32);
        if (!crypto::aeadDecrypt(timestamp, key, 0, msg + 88, 12, hash, 32)) break;
        mixHash(hash, msg + 88, 28);
        ok = true;
    } while (0);

    if (!ok) {
        stats_.rxHandshakeFail++;
        goto out;
    }
    if (memcmp(timestamp, p->lastTimestamp, 12) <= 0 && !timestampForgotten(p, now)) {
        stats_.rxStaleTimestamp++;
        goto out;
    }
    if (p->initiationReceived && (now - p->lastInitiationRxMs) < kMinInitiationInterval) {
        stats_.rxFlood++;
        goto out;
    }
    memcpy(p->lastTimestamp, timestamp, 12);
    p->initiationReceived = true;
    p->lastInitiationRxMs = now;
    p->endpoint = from;
    anyReceived(p, now);

    {
        // Our own pending initiation, if any, is superseded by the peer's.
        crypto::wipe(&p->handshake, sizeof(p->handshake));

        uint8_t ephemeralPrivate[32], ephemeralPublic[32], tau[32];
        uint8_t *resp = tx_;
        Session &s = p->next;
        clearSession(&s);

        ok = randomBytes(ephemeralPrivate, 32);
        if (ok) {
            crypto::clampPrivateKey(ephemeralPrivate);
            crypto::publicKey(ephemeralPublic, ephemeralPrivate);
            setHeader(resp, kTypeResponse);
            s.localIndex = newIndex();
            s.remoteIndex = get32(msg + 4);
            put32(resp + 4, s.localIndex);
            put32(resp + 8, s.remoteIndex);
            memcpy(resp + 12, ephemeralPublic, 32);
            mixHash(hash, ephemeralPublic, 32);
            crypto::kdf(chainKey, nullptr, nullptr, chainKey, ephemeralPublic, 32);
            ok = crypto::dh(dhOut, ephemeralPrivate, ephemeralI);
        }
        if (ok) {
            crypto::kdf(chainKey, nullptr, nullptr, chainKey, dhOut, 32);
            ok = crypto::dh(dhOut, ephemeralPrivate, p->publicKey);
        }
        if (ok) {
            crypto::kdf(chainKey, nullptr, nullptr, chainKey, dhOut, 32);
            crypto::kdf(chainKey, tau, key, chainKey, p->presharedKey, 32);
            mixHash(hash, tau, 32);
            static const uint8_t empty[1] = {0};
            crypto::aeadEncrypt(resp + 44, key, 0, empty, 0, hash, 32);
            mixHash(hash, resp + 44, 16);
            addMacs(p, resp, kResponseLen, now);

            // The responder receives with the first key and sends with the second.
            crypto::kdf(s.recvKey, s.sendKey, nullptr, chainKey, nullptr, 0);
            s.valid = true;
            s.initiator = false;
            s.birthMs = now;
            s.sendCounter = 0;
            s.replay.reset();
        }
        crypto::wipe(ephemeralPrivate, sizeof(ephemeralPrivate));
        crypto::wipe(tau, sizeof(tau));
        if (ok) {
            transport_->sendOuter(p->endpoint, resp, kResponseLen);
            stats_.handshakesResponded++;
            sessionDerived(p, now);
            anySent(p, now);
        } else {
            clearSession(&s);
            stats_.rxHandshakeFail++;
        }
    }

out:
    crypto::wipe(chainKey, sizeof(chainKey));
    crypto::wipe(hash, sizeof(hash));
    crypto::wipe(key, sizeof(key));
    crypto::wipe(dhOut, sizeof(dhOut));
}

void Device::handleResponse(uint8_t *msg, size_t len, const Endpoint &from, uint32_t now)
{
    if (len != kResponseLen) {
        stats_.rxShort++;
        return;
    }
    uint8_t expected[16];
    crypto::mac(expected, mac1Key_, 32, msg, kResponseLen - 32);
    if (!crypto::equal16(expected, msg + 60)) {
        stats_.rxBadMac1++;
        return;
    }

    uint32_t receiver = get32(msg + 8);
    Peer *p = nullptr;
    for (int i = 0; i < TINYWG_MAX_PEERS; i++) {
        Peer &c = peers_[i];
        if (c.used && c.handshake.initiationSent && c.handshake.localIndex == receiver) {
            p = &c;
            break;
        }
    }
    if (!p) {
        stats_.rxNoSession++;
        return;
    }

    detail::Handshake &hs = p->handshake;
    uint8_t chainKey[32], hash[32], key[32], tau[32], dhOut[32];
    const uint8_t *ephemeralR = msg + 12;
    bool ok;

    memcpy(chainKey, hs.chainKey, 32);
    memcpy(hash, hs.hash, 32);
    mixHash(hash, ephemeralR, 32);
    crypto::kdf(chainKey, nullptr, nullptr, chainKey, ephemeralR, 32);
    ok = crypto::dh(dhOut, hs.ephemeralPrivate, ephemeralR);
    if (ok) {
        crypto::kdf(chainKey, nullptr, nullptr, chainKey, dhOut, 32);
        ok = crypto::dh(dhOut, staticPrivate_, ephemeralR);
    }
    if (ok) {
        crypto::kdf(chainKey, nullptr, nullptr, chainKey, dhOut, 32);
        crypto::kdf(chainKey, tau, key, chainKey, p->presharedKey, 32);
        mixHash(hash, tau, 32);
        uint8_t empty[1];
        ok = crypto::aeadDecrypt(empty, key, 0, msg + 44, 0, hash, 32);
    }

    if (ok) {
        Session fresh;
        memset(&fresh, 0, sizeof(fresh));
        crypto::kdf(fresh.sendKey, fresh.recvKey, nullptr, chainKey, nullptr, 0);
        fresh.valid = true;
        fresh.initiator = true;
        fresh.localIndex = hs.localIndex;
        fresh.remoteIndex = get32(msg + 4);
        fresh.birthMs = now;
        fresh.replay.reset();
        crypto::wipe(&hs, sizeof(hs));

        // An unconfirmed responder session loses to the one just completed.
        if (p->next.valid) {
            clearSession(&p->previous);
            p->previous = p->next;
            clearSession(&p->next);
            clearSession(&p->current);
        } else {
            clearSession(&p->previous);
            p->previous = p->current;
        }
        p->current = fresh;
        crypto::wipe(&fresh, sizeof(fresh));

        p->endpoint = from;
        anyReceived(p, now);
        heardFrom(p, now);
        sessionDerived(p, now);
        handshakeComplete(p, now);
        // Confirm the session to the responder: staged data, or a keepalive.
        sendStaged(p, true, now);
    } else {
        stats_.rxHandshakeFail++;
    }
    crypto::wipe(chainKey, sizeof(chainKey));
    crypto::wipe(hash, sizeof(hash));
    crypto::wipe(key, sizeof(key));
    crypto::wipe(tau, sizeof(tau));
    crypto::wipe(dhOut, sizeof(dhOut));
}

// Whitepaper §5.4.7: the responder is under load and wants mac2.
void Device::handleCookieReply(uint8_t *msg, size_t len, uint32_t now)
{
    if (len != kCookieLen) {
        stats_.rxShort++;
        return;
    }
    uint32_t receiver = get32(msg + 4);
    for (int i = 0; i < TINYWG_MAX_PEERS; i++) {
        Peer &p = peers_[i];
        if (!p.used || !p.hasLastMac1) continue;
        bool match = (p.handshake.initiationSent && p.handshake.localIndex == receiver) ||
                     (p.next.valid && p.next.localIndex == receiver);
        if (!match) continue;
        uint8_t cookie[16];
        if (crypto::xaeadDecrypt(cookie, p.cookieKey, msg + 8, msg + 32, 16, p.lastMac1, 16)) {
            memcpy(p.cookie, cookie, 16);
            p.hasCookie = true;
            p.cookieMs = now;
            p.hasLastMac1 = false;
        } else {
            stats_.rxHandshakeFail++;
        }
        return;
    }
    stats_.rxNoSession++;
}

// ---- transport data (whitepaper §5.4.6)

void Device::handleData(uint8_t *msg, size_t len, const Endpoint &from, uint32_t now)
{
    if (len < kDataMinLen) {
        stats_.rxShort++;
        return;
    }
    uint32_t receiver = get32(msg + 4);
    uint64_t counter = get64(msg + 8);
    Peer *p = nullptr;
    Session *s = nullptr;
    for (int i = 0; i < TINYWG_MAX_PEERS && !s; i++) {
        Peer &c = peers_[i];
        if (!c.used) continue;
        Session *cands[3] = {&c.current, &c.previous, &c.next};
        for (Session *x : cands) {
            if (x->valid && x->localIndex == receiver) {
                p = &c;
                s = x;
                break;
            }
        }
    }
    if (!s || (now - s->birthMs) >= kRejectAfterTime) {
        stats_.rxNoSession++;
        return;
    }
    if (!s->replay.acceptable(counter)) {
        stats_.rxReplay++;
        return;
    }
    size_t plainLen = len - kDataMinLen;
    uint8_t *plain = msg + kDataHeaderLen;
    if (!crypto::aeadDecrypt(plain, s->recvKey, counter, plain, plainLen, nullptr, 0)) {
        stats_.rxDecryptFail++;
        return;
    }
    s->replay.mark(counter);
    p->endpoint = from;
    p->rxBytes += len;
    anyReceived(p, now);
    heardFrom(p, now);

    if (s == &p->next) {
        // The initiator's first data message confirms the session we responded with.
        clearSession(&p->previous);
        p->previous = p->current;
        p->current = p->next;
        memset(&p->next, 0, sizeof(p->next));
        s = &p->current;
        handshakeComplete(p, now);
        sendStaged(p, false, now);
    }

    // Initiator side: rekey before the session is rejected under our feet.
    if (s == &p->current && s->initiator && !p->lastMinuteHandshake &&
        (now - s->birthMs) >= kRejectAfterTime - kKeepaliveTimeout - kRekeyTimeout) {
        p->lastMinuteHandshake = true;
        initiateHandshake(p, false, now);
    }

    if (plainLen == 0) return;   // keepalive

    // The inner packet must be IPv4 from an address this peer may use.
    if ((plain[0] >> 4) != 4 || plainLen < 20) {
        stats_.rxNotIpv4++;
        return;
    }
    size_t headerLen = (size_t)(plain[0] & 0x0F) * 4;
    size_t totalLen = ((size_t)plain[2] << 8) | plain[3];
    if (headerLen < 20 || totalLen < headerLen || totalLen > plainLen) {
        stats_.rxNotIpv4++;
        return;
    }
    if (routePeer(plain + 12) != indexOf(p)) {
        stats_.rxNotAllowed++;
        return;
    }
    if (!p->passiveKeepalive.armed) p->passiveKeepalive.arm(now + kKeepaliveTimeout);
    transport_->deliverInner(indexOf(p), plain, totalLen);
}

SendResult Device::sendData(Peer *p, const uint8_t *packet, size_t len, uint32_t now)
{
    Session &s = p->current;
    if (!sessionUsable(s, now)) {
#if TINYWG_STAGED_PACKET
        if (len > 0) {
            stagedPeer_ = indexOf(p);
            stagedLen_ = len;
            memcpy(staged_, packet, len);
        }
#endif
        if (p->endpoint.port == 0) return SendResult::NoEndpoint;
        if (!p->attempting) initiateHandshake(p, false, now);
#if TINYWG_STAGED_PACKET
        return len > 0 ? SendResult::Queued : SendResult::Dropped;
#else
        return SendResult::Dropped;
#endif
    }
    if (p->endpoint.port == 0) return SendResult::NoEndpoint;

    // Pad to a multiple of 16 without exceeding the MTU (whitepaper §5.4.6).
    size_t padded = (len + 15) & ~(size_t)15;
    if (padded > mtu_) padded = len;
    uint8_t *msg = tx_;
    uint64_t counter = s.sendCounter++;
    setHeader(msg, kTypeData);
    put32(msg + 4, s.remoteIndex);
    put64(msg + 8, counter);
    if (len) memcpy(msg + kDataHeaderLen, packet, len);
    memset(msg + kDataHeaderLen + len, 0, padded - len);
    crypto::aeadEncrypt(msg + kDataHeaderLen, s.sendKey, counter, msg + kDataHeaderLen, padded, nullptr, 0);
    size_t total = kDataHeaderLen + padded + crypto::kTagSize;
    transport_->sendOuter(p->endpoint, msg, total);
    p->txBytes += total;

    anySent(p, now);
    if (len > 0 && !p->newHandshake.armed) p->newHandshake.arm(now + kKeepaliveTimeout + kRekeyTimeout);
    if (s.sendCounter >= kRekeyAfterMessages || (s.initiator && (now - s.birthMs) >= kRekeyAfterTime))
        initiateHandshake(p, false, now);
    return SendResult::Sent;
}

// Sends the staged packet for p; when there is none, a keepalive if asked for.
void Device::sendStaged(Peer *p, bool keepaliveIfEmpty, uint32_t now)
{
#if TINYWG_STAGED_PACKET
    if (stagedPeer_ == indexOf(p)) {
        stagedPeer_ = -1;
        sendData(p, staged_, stagedLen_, now);
        return;
    }
#endif
    if (keepaliveIfEmpty) sendData(p, nullptr, 0, now);
}

// ---- public entry points

void Device::receiveOuter(uint8_t *data, size_t len, const Endpoint &from, uint32_t nowMs)
{
    if (!started_) return;
    if (len < 4 || data[1] || data[2] || data[3]) {
        stats_.rxShort++;
        return;
    }
    switch (data[0]) {
    case kTypeInitiation: handleInitiation(data, len, from, nowMs); break;
    case kTypeResponse: handleResponse(data, len, from, nowMs); break;
    case kTypeCookie: handleCookieReply(data, len, nowMs); break;
    case kTypeData: handleData(data, len, from, nowMs); break;
    default: stats_.rxShort++; break;
    }
}

SendResult Device::sendInner(const uint8_t *packet, size_t len, uint32_t nowMs)
{
    if (!started_ || len < 20 || len > mtu_ || (packet[0] >> 4) != 4) return SendResult::Invalid;
    int index = routePeer(packet + 16);
    if (index < 0) return SendResult::NoRoute;
    return sendData(&peers_[index], packet, len, nowMs);
}

bool Device::connect(int index, uint32_t nowMs)
{
    Peer *p = peer(index);
    if (!p || !started_) return false;
    return initiateHandshake(p, false, nowMs);
}

void Device::tick(uint32_t now)
{
    if (!started_) return;
    for (int i = 0; i < TINYWG_MAX_PEERS; i++) {
        Peer *p = &peers_[i];
        if (!p->used) continue;

        if (p->attempting && p->retransmit.expired(now)) {
            p->retransmit.disarm();
            if ((now - p->attemptStartMs) >= kRekeyAttemptTime) {
                // Give up until something new is sent (whitepaper §6.4).
                p->attempting = false;
                crypto::wipe(&p->handshake, sizeof(p->handshake));
#if TINYWG_STAGED_PACKET
                if (stagedPeer_ == i) stagedPeer_ = -1;
#endif
            } else {
                initiateHandshake(p, true, now);
            }
        }
        if (p->passiveKeepalive.expired(now)) {
            p->passiveKeepalive.disarm();
            if (sessionUsable(p->current, now)) sendData(p, nullptr, 0, now);
        }
        if (p->newHandshake.expired(now)) {
            p->newHandshake.disarm();
            initiateHandshake(p, false, now);
        }
        if (p->persistentKeepalive.expired(now) || p->keepaliveNow) {
            p->persistentKeepalive.disarm();
            p->keepaliveNow = false;
            sendData(p, nullptr, 0, now);
        }
        if (p->zeroKeys.expired(now)) {
            p->zeroKeys.disarm();
            wipePeerKeys(p);
        }
        if (p->hasCookie && (now - p->cookieMs) >= kCookieLifetime) p->hasCookie = false;
        // Before the 32-bit millisecond clock wraps and makes it look recent again.
        if (p->heardFrom && (now - p->lastHeardMs) >= 0x7fffffffu) p->heardFrom = false;
    }
}

}  // namespace tinywg
