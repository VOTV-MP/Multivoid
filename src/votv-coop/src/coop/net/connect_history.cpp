// coop/net/connect_history.cpp -- see coop/net/connect_history.h.

#include "coop/net/connect_history.h"

#include "ue_wrap/core/log.h"

#pragma warning(push)
#pragma warning(disable: 4100 4127 4191 4244 4245 4267 4310 4324 4458)
#include <steam/steamnetworkingtypes.h>
#include <steam/isteamnetworkingsockets.h>
#pragma warning(pop)

#include <cstring>

namespace coop::net::connect_history {

bool KeyFromAddressBytes(const uint8_t ipv6[16], Key& out) {
    static const uint8_t kZero[16] = {};
    if (std::memcmp(ipv6, kZero, sizeof(kZero)) == 0) return false;
    std::memcpy(out.bytes, ipv6, sizeof(out.bytes));
    out.kind = KeyKind::Address;
    return true;
}

bool KeyFromAddress(uint32_t hConn, Key& out) {
    auto* sockets = SteamNetworkingSockets();
    if (!sockets) return false;
    SteamNetConnectionInfo_t info{};
    if (!sockets->GetConnectionInfo(static_cast<HSteamNetConnection>(hConn), &info))
        return false;
    return KeyFromAddressBytes(info.m_addrRemote.m_ipv6, out);
}

Key KeyFromIdentity(const peer_identity::PubKey& pub) {
    Key k;
    std::memcpy(k.bytes, pub.data(), sizeof(k.bytes));
    k.kind = KeyKind::Identity;
    return k;
}

// --- History ---------------------------------------------------------------

void History::SetPolicy(Policy p) {
    if (p.max < 0) p.max = 0;
    if (p.max > kMaxStamps) p.max = kMaxStamps;
    policy_ = p;
    Clear();
}

void History::Clear() {
    for (auto& b : buckets_) b = Bucket{};
}

bool History::IsLive(const Bucket& b, uint64_t nowMs) const {
    if (!b.used) return false;
    if (nowMs < b.ignoreUntilMs) return true;
    return b.count > 0 && nowMs - b.newestMs < policy_.windowMs;
}

void History::Expire(Bucket& b, uint64_t nowMs) {
    // The ring holds stamps in arrival order, oldest at head; a stamp that has left the window
    // takes every older one with it.
    while (b.count > 0 && nowMs - b.stamps[b.head] >= policy_.windowMs) {
        b.head = (b.head + 1) % kMaxStamps;
        --b.count;
    }
}

History::Bucket* History::Find(const Key& key, uint64_t nowMs) {
    Bucket* freeRow = nullptr;
    for (auto& b : buckets_) {
        if (b.used && b.key.kind == key.kind &&
            std::memcmp(b.key.bytes, key.bytes, sizeof(key.bytes)) == 0) {
            return &b;
        }
        // An expired row is reusable and a free one no better; both are collected in one pass,
        // so a table of stale rows never reads as full.
        if (!freeRow && !IsLive(b, nowMs)) freeRow = &b;
    }
    if (!freeRow) {
        if (name_ && !fullLogged_) {
            UE_LOGW("connect_history: the %s table is full of live sources (%d rows) -- a new "
                    "source is let through uncounted until a row expires", name_, kBuckets);
            fullLogged_ = true;
        }
        return nullptr;
    }
    fullLogged_ = false;
    *freeRow = Bucket{};
    freeRow->key = key;
    freeRow->used = true;
    return freeRow;
}

Verdict History::Blocked(const Key& key, uint64_t nowMs) {
    Verdict v;
    if (policy_.max <= 0) return v;
    Bucket* b = Find(key, nowMs);
    if (!b) {
        v.count = -1;
        return v;
    }
    if (nowMs < b->ignoreUntilMs) {
        v.refused = true;
        v.count = b->count;
        v.retryMs = b->ignoreUntilMs - nowMs;
        return v;
    }
    Expire(*b, nowMs);
    v.count = b->count;
    // MTA's own check is `>` (CConnectHistory.cpp:53), which admits one event more than its
    // constant names; the count here means what it says, so the fifth connection under a cap
    // of four is the refused one.
    if (b->count >= policy_.max) {
        b->ignoreUntilMs = nowMs + policy_.ignoreMs;
        v.refused = true;
        v.retryMs = policy_.ignoreMs;
    }
    return v;
}

void History::Record(const Key& key, uint64_t nowMs) {
    if (policy_.max <= 0) return;
    Bucket* b = Find(key, nowMs);
    if (!b) return;
    Expire(*b, nowMs);
    // A ring full of live stamps already holds the policy's max (the max is clamped to the
    // ring), so a further stamp changes no verdict and is not kept.
    if (b->count >= kMaxStamps) return;
    b->stamps[(b->head + b->count) % kMaxStamps] = nowMs;
    ++b->count;
    b->newestMs = nowMs;
}

Verdict History::Note(const Key& key, uint64_t nowMs) {
    const Verdict v = Blocked(key, nowMs);
    if (!v.refused && v.count >= 0) Record(key, nowMs);
    return v;
}

int History::LiveRows(uint64_t nowMs) const {
    int n = 0;
    for (const auto& b : buckets_)
        if (IsLive(b, nowMs)) ++n;
    return n;
}

// --- the connection cap ------------------------------------------------------

History& Connects() {
    static History h{Policy{}, "connection cap"};
    return h;
}

void ConfigureConnects(long cap, long windowS) {
    Policy p;
    p.max = static_cast<int>(cap);
    p.windowMs = static_cast<uint64_t>(windowS > 0 ? windowS : 1) * 1000;
    p.ignoreMs = p.windowMs;
    Connects().SetPolicy(p);
    const Policy& live = Connects().GetPolicy();
    if (live.max > 0) {
        UE_LOGI("connect_history: cap %d connections per %llu s per address, then %llu s "
                "of refusal (net.connect_cap, net.connect_window_s)", live.max,
                static_cast<unsigned long long>(live.windowMs / 1000),
                static_cast<unsigned long long>(live.ignoreMs / 1000));
    } else {
        UE_LOGW("connect_history: the connection cap is OFF (net.connect_cap=0) -- one address "
                "may open connections without limit");
    }
}

// --- the selftest ------------------------------------------------------------

namespace {

Key AddrKey(uint8_t tag) {
    Key k;
    std::memset(k.bytes, 0, sizeof(k.bytes));
    k.bytes[0] = 0xC0;
    k.bytes[15] = tag;
    k.kind = KeyKind::Address;
    return k;
}

}  // namespace

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("connect_history selftest FAIL: %s", what);
    };

    // 1. MTA's join-flood numbers: four inside thirty seconds pass, the fifth is refused, the
    // refusal lasts one window, and nothing a refused source does extends it.
    {
        History h{Policy{4, 30'000, 30'000}};
        const Key a = AddrKey(1);
        bool allowed = true;
        for (uint64_t t = 1000; t <= 4000; t += 1000) allowed = allowed && !h.Note(a, t).refused;
        check(allowed, "four connections inside the window pass");
        Verdict v = h.Note(a, 5000);
        check(v.refused && v.count == 4 && v.retryMs == 30'000,
              "the fifth is refused for one window");
        v = h.Note(a, 20'000);
        check(v.refused && v.retryMs == 15'000,
              "a refused source stays refused, and the refusal is not extended");
        check(h.Note(a, 34'999).refused, "refused up to the last millisecond");
        v = h.Note(a, 35'000);
        check(!v.refused && v.count == 0, "the refusal lifts with a clean row");
        check(!h.Note(a, 36'000).refused && !h.Note(a, 37'000).refused &&
                  !h.Note(a, 38'000).refused && h.Note(a, 39'000).refused,
              "the window counts again from the clean row");
    }

    // 2. The window slides: two events inside it both count while both are inside it, and the
    // older one leaving is what re-admits the source.
    {
        History h{Policy{2, 10'000, 0}};
        const Key a = AddrKey(2);
        check(!h.Note(a, 0).refused && !h.Note(a, 5000).refused,
              "two events inside the window pass");
        check(h.Note(a, 6000).refused, "the third inside the window is refused");
        check(h.Note(a, 9999).refused, "still refused while both stamps are inside the window");
        Verdict v = h.Note(a, 10'000);
        check(!v.refused && v.count == 1,
              "the oldest stamp leaving the window re-admits the source");
    }

    // 3. Sources do not share rows: a second address, and the same bytes under the other kind.
    {
        History h{Policy{1, 10'000, 10'000}};
        const Key a = AddrKey(3);
        Key sameBytesIdentity = a;
        sameBytesIdentity.kind = KeyKind::Identity;
        check(!h.Note(a, 0).refused && h.Note(a, 1).refused, "one source hits its own cap");
        check(!h.Note(AddrKey(4), 2).refused, "another address is unaffected");
        check(!h.Note(sameBytesIdentity, 3).refused,
              "the same bytes under the other kind are another source");
    }

    // 4. The guess shape: ask, then record only a failure. Ten failures refuse the eleventh
    // attempt, and the refusal lasts the minute.
    {
        History h{Policy{10, 60'000, 60'000}};
        const Key a = AddrKey(5);
        bool open = true;
        for (uint64_t t = 0; t < 10; ++t) {
            open = open && !h.Blocked(a, t).refused;
            h.Record(a, t);
        }
        check(open, "ten asks before ten failures are all open");
        check(h.Blocked(a, 10).refused, "the eleventh ask after ten failures is refused");
        check(h.Blocked(a, 60'009).refused, "refused for the whole minute");
        check(!h.Blocked(a, 60'010).refused, "open again after the minute");
    }

    // 5. A full table refuses nobody: sixty-four live sources, then one more that cannot be
    // bucketed and is let through uncounted; once the rows expire it gets a row.
    {
        History h{Policy{1, 10'000, 10'000}};
        for (int i = 0; i < History::kBuckets; ++i) {
            Key k = AddrKey(static_cast<uint8_t>(i));
            k.bytes[1] = 0x55;
            h.Note(k, 0);
        }
        check(h.LiveRows(0) == History::kBuckets, "sixty-four live rows fill the table");
        Key extra = AddrKey(200);
        extra.bytes[1] = 0x66;
        Verdict v = h.Note(extra, 1);
        check(!v.refused && v.count == -1, "a source with no row is let through, uncounted");
        v = h.Note(extra, 2);
        check(!v.refused && v.count == -1, "and again, since nothing was counted");
        v = h.Note(extra, 10'000);
        check(!v.refused && v.count == 0 && h.Note(extra, 10'001).refused,
              "once the rows expire the source gets one and its cap applies");
    }

    // 6. A max of zero refuses nothing; a max above the ring is clamped to it.
    {
        History off{Policy{0, 10'000, 10'000}};
        bool open = true;
        for (uint64_t t = 0; t < 100; ++t) open = open && !off.Note(AddrKey(6), t).refused;
        check(open && off.LiveRows(0) == 0, "a cap of zero refuses nothing and keeps nothing");
        History big{Policy{100, 10'000, 10'000}};
        check(big.GetPolicy().max == History::kMaxStamps,
              "a cap above the ring is clamped to it");
        open = true;
        for (uint64_t t = 0; t < static_cast<uint64_t>(History::kMaxStamps); ++t)
            open = open && !big.Note(AddrKey(7), t).refused;
        check(open && big.Note(AddrKey(7), 100).refused,
              "the clamped cap holds exactly that many");
    }

    // 7. The identity key is the first half of the public key under its own kind; an all-zero
    // address is no key and a mapped IPv4 one is.
    {
        peer_identity::PubKey pub{};
        for (size_t i = 0; i < pub.size(); ++i) pub[i] = static_cast<uint8_t>(i + 1);
        const Key k = KeyFromIdentity(pub);
        check(k.kind == KeyKind::Identity && std::memcmp(k.bytes, pub.data(), 16) == 0,
              "an identity key is the first sixteen bytes of the public key");
        uint8_t addrBytes[16] = {};
        Key addr;
        check(!KeyFromAddressBytes(addrBytes, addr), "an all-zero address is no key");
        addrBytes[10] = 0xFF; addrBytes[11] = 0xFF; addrBytes[12] = 127; addrBytes[15] = 1;
        check(KeyFromAddressBytes(addrBytes, addr) && addr.kind == KeyKind::Address &&
                  std::memcmp(addr.bytes, addrBytes, 16) == 0,
              "a mapped IPv4 address is a key of the address kind");
    }

    if (pass == total) {
        UE_LOGI("connect_history selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("connect_history selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::connect_history
