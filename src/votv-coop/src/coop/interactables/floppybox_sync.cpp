// coop/interactables/floppybox_sync.cpp -- see the header. v121 (OPEN-10).

#include "coop/interactables/floppybox_sync.h"

#include "coop/comms/chat_feed.h"  // ToUtf8
#include "coop/element/registry.h"
#include "coop/net/blob_chunks.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_lifecycle.h"  // DestroyLocalProp (deny reap)
#include "coop/session/net_pump.h"      // HasAnnouncedWorldReady

#include "ue_wrap/devices/floppybox.h"
#include "ue_wrap/devices/laptop.h"     // IsDiscClass (the reap target gate)
#include "ue_wrap/engine/engine.h"      // ReadMainPlayerGrabState
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace coop::floppybox_sync {
namespace {

namespace FB = ue_wrap::floppybox;
namespace R = ue_wrap::reflection;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr uint64_t kSweepMs = 1000;      // 1 Hz (the rack cadence)
constexpr uint64_t kDenyTtlMs = 10000;   // reap correlation window
constexpr uint64_t kPendingTtlMs = 30000;

uint64_t NowMs() { return static_cast<uint64_t>(::GetTickCount64()); }

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

uint64_t HashStr(const std::wstring& w) {
    const std::string u = coop::chat_feed::ToUtf8(w);
    return coop::blob_chunks::Fnv64(reinterpret_cast<const uint8_t*>(u.data()), u.size());
}

// ---- wire ----
// head [u8 op][u32 eid]; op 0=push{[i32 type][u16 len][utf8]}, 1=pop{[u64 hash]},
// 2=deny{[u64 hash]}, 3=canonical{[u16 n]{[i32 type][u16 len][utf8]}}
void PutU16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
}
void PutStr(std::vector<uint8_t>& b, const std::wstring& w) {
    std::string u = coop::chat_feed::ToUtf8(w);
    if (u.size() > 0xFFFF) u.resize(0xFFFF);
    PutU16(b, static_cast<uint16_t>(u.size()));
    b.insert(b.end(), u.begin(), u.end());
}

struct Reader {
    const uint8_t* p; size_t n, off = 0;
    bool ok = true;
    uint8_t U8() { if (off + 1 > n) { ok = false; return 0; } return p[off++]; }
    uint16_t U16() { if (off + 2 > n) { ok = false; return 0; } uint16_t v = static_cast<uint16_t>(p[off] | (p[off+1] << 8)); off += 2; return v; }
    uint32_t U32() { if (off + 4 > n) { ok = false; return 0; } uint32_t v = 0; std::memcpy(&v, p + off, 4); off += 4; return v; }
    uint64_t U64() { if (off + 8 > n) { ok = false; return 0; } uint64_t v = 0; std::memcpy(&v, p + off, 8); off += 8; return v; }
    std::wstring Str() {
        const uint16_t len = U16();
        if (!ok || off + len > n) { ok = false; return std::wstring(); }
        std::string u(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return FromUtf8(u);
    }
};

std::vector<uint8_t> Head(uint8_t op, uint32_t eid) {
    std::vector<uint8_t> b;
    b.push_back(op);
    PutU32(b, eid);
    return b;
}

// ---- state ----
struct Shadow {
    FB::BoxArrays arrays;
    uint64_t digest = 0;  // FB::ReadDigest of `arrays` as last seen (pre-filter)
};
std::map<uint32_t, Shadow> g_shadow;   // eid -> last known arrays + digest
struct PendingCanonical { std::vector<uint8_t> blob; uint64_t deadline; };
std::map<uint32_t, PendingCanonical> g_pending;  // unresolved-eid canonicals
struct Taken { uint32_t eid; uint64_t hash; uint64_t when; };
Taken g_takenRing[8] = {};
int g_takenNext = 0;
coop::blob_chunks::Assembler g_asm;
uint32_t g_nextSeq = 1;
uint64_t g_nextSweep = 0;
std::set<uint32_t> g_canonRetry;  // CRIT-1: eids whose canonical send was refused
bool g_announced = false;

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

using coop::element::LivePropActor;  // the promoted canonical eid->live-Prop resolve

std::vector<uint8_t> PackCanonical(uint32_t eid, const FB::BoxArrays& a) {
    std::vector<uint8_t> b = Head(3, eid);
    const size_t n = a.types.size() < a.data.size() ? a.types.size() : a.data.size();
    PutU16(b, static_cast<uint16_t>(n));
    for (size_t i = 0; i < n; ++i) {
        PutU32(b, static_cast<uint32_t>(a.types[i]));
        PutStr(b, a.data[i]);
    }
    return b;
}

// CRIT-1: bound the canonical below the blob transport cap (drop TAIL entries
// + WARN -- the OPEN-9-class accepted residual; the box tail is the LIFO top).
std::vector<uint8_t> PackCanonicalBounded(uint32_t eid, FB::BoxArrays a) {
    std::vector<uint8_t> b = PackCanonical(eid, a);
    int dropped = 0;
    while (b.size() > coop::blob_chunks::MaxBlobBytes() && !a.data.empty()) {
        a.data.pop_back();
        if (!a.types.empty()) a.types.pop_back();
        ++dropped;
        b = PackCanonical(eid, a);
    }
    if (dropped)
        UE_LOGW("floppybox: canonical eid=%u over the %zu B transport cap -- %d tail "
                "entr(ies) dropped (OPEN-9-class residual)", eid,
                coop::blob_chunks::MaxBlobBytes(), dropped);
    return b;
}

bool ParseCanonical(Reader& r, FB::BoxArrays& a) {
    const uint16_t n = r.U16();
    for (uint16_t i = 0; i < n && r.ok; ++i) {
        a.types.push_back(static_cast<int32_t>(r.U32()));
        a.data.push_back(r.Str());
    }
    return r.ok;
}

void PrimeShadow(uint32_t eid, void* actor, FB::BoxArrays&& a) {
    Shadow sh;
    sh.arrays = std::move(a);
    FB::ReadDigest(actor, sh.digest);  // digest of the LIVE state just read/written
    g_shadow[eid] = std::move(sh);
}

bool AnyClientReady(coop::net::Session* s) {
    for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers); ++slot)
        if (s->IsSlotWorldReady(slot)) return true;
    return false;
}

void HostBroadcastCanonical(coop::net::Session* s, uint32_t eid, void* actor) {
    FB::BoxArrays a;
    if (!FB::ReadArrays(actor, a)) return;
    // No READY client -> prime silently (the joiner gets the connect canonical
    // at its ready edge; smoke 1 caught the load-window refused spam class).
    if (!AnyClientReady(s)) {
        PrimeShadow(eid, actor, std::move(a));
        g_canonRetry.erase(eid);
        return;
    }
    // CRIT-1: refused send must not prime; the sweep retry set re-sends.
    if (coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppyBoxState,
                                    g_nextSeq++, PackCanonicalBounded(eid, a))) {
        PrimeShadow(eid, actor, std::move(a));
        g_canonRetry.erase(eid);
    } else {
        if (!g_canonRetry.count(eid))
            UE_LOGW("floppybox: canonical eid=%u send refused -- retry armed (1 Hz sweep)", eid);
        g_canonRetry.insert(eid);
    }
}

void ApplyCanonicalTo(void* actor, uint32_t eid, const FB::BoxArrays& a) {
    FB::BoxArrays cur;
    if (FB::ReadArrays(actor, cur) &&
        cur.types == a.types && cur.data == a.data) {
        PrimeShadow(eid, actor, std::move(cur));  // equal: prime only
        return;
    }
    FB::WriteArraysAndGen(actor, a);
    FB::BoxArrays copy = a;
    PrimeShadow(eid, actor, std::move(copy));
    UE_LOGI("floppybox: canonical adopted (eid=%u, %zu disc(s))", eid, a.data.size());
}

// The 1 Hz sweep: derive tail ops (client) / canonical on change (host).
void Sweep(coop::net::Session* s, uint64_t now) {
    if (now < g_nextSweep) return;
    g_nextSweep = now + kSweepMs;

    g_asm.Sweep(Clock::now(), std::chrono::seconds(10));  // 1 Hz per the blob_chunks contract

    // CRIT-1 retry: re-send refused canonicals (host only; erased on success).
    if (IsHost() && !g_canonRetry.empty()) {
        std::set<uint32_t> retry = g_canonRetry;
        for (uint32_t eid : retry) {
            void* actor = LivePropActor(eid);
            if (!actor || !FB::IsFloppyBoxClass(R::ClassOf(actor))) {
                g_canonRetry.erase(eid);
                continue;
            }
            HostBroadcastCanonical(s, eid, actor);
        }
    }

    // Drain unresolved-eid canonicals (birth skew).
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        void* actor = LivePropActor(it->first);
        if (actor && FB::IsFloppyBoxClass(R::ClassOf(actor))) {
            Reader r{it->second.blob.data(), it->second.blob.size()};
            FB::BoxArrays a;
            if (ParseCanonical(r, a)) ApplyCanonicalTo(actor, it->first, a);
            it = g_pending.erase(it);
            continue;
        }
        if (now > it->second.deadline) {
            UE_LOGW("floppybox: pending canonical eid=%u EXPIRED (birth lane skew)", it->first);
            it = g_pending.erase(it);
            continue;
        }
        ++it;
    }

    const bool host = IsHost();
    const bool clientReady = host || coop::net_pump::HasAnnouncedWorldReady();
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!FB::IsFloppyBoxClass(R::ClassOf(pr.actor))) continue;
        const uint32_t eid = static_cast<uint32_t>(pr.id);
        // Alloc-free digest pre-filter (perf audit F1): full ReadArrays only
        // when the raw-buffer digest moved vs the shadow.
        uint64_t dig = 0;
        if (!FB::ReadDigest(pr.actor, dig)) continue;
        auto it = g_shadow.find(eid);
        if (it != g_shadow.end() && it->second.digest == dig) continue;
        FB::BoxArrays cur;
        if (!FB::ReadArrays(pr.actor, cur)) continue;
        if (it == g_shadow.end()) {  // first sight: silent prime
            PrimeShadow(eid, pr.actor, std::move(cur));
            continue;
        }
        FB::BoxArrays& prev = it->second.arrays;
        if (prev.types == cur.types && prev.data == cur.data) {
            it->second.digest = dig;  // content identical, digest churn only
            continue;
        }
        if (!s->connected() || !clientReady) {
            PrimeShadow(eid, pr.actor, std::move(cur));
            continue;
        }
        if (host) {
            UE_LOGI("floppybox: host organic change (eid=%u) -- canonical broadcast", eid);
            HostBroadcastCanonical(s, eid, pr.actor);
            continue;
        }
        // Client: derive TAIL ops -- common prefix p; pops for prev[p..]
        // (reverse order), pushes for cur[p..] (in order). LIFO grammar.
        // Perf audit F3: the shadow advances INCREMENTALLY per ACCEPTED send
        // (blob_chunks I-3) -- a refused op stays underived-for and re-derives
        // next sweep; no op is silently lost, no dupe is re-sent.
        size_t p = 0;
        while (p < prev.data.size() && p < cur.data.size() &&
               prev.data[p] == cur.data[p] &&
               p < prev.types.size() && p < cur.types.size() &&
               prev.types[p] == cur.types[p]) ++p;
        bool refused = false;
        for (size_t i = prev.data.size(); i > p && !refused; --i) {
            const uint64_t h = HashStr(prev.data[i - 1]);
            std::vector<uint8_t> b = Head(1, eid);
            PutU64(b, h);
            if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppyBoxState,
                                             g_nextSeq++, b)) {
                refused = true;
                break;
            }
            g_takenRing[g_takenNext] = {eid, h, now};
            g_takenNext = (g_takenNext + 1) % 8;
            prev.types.erase(prev.types.begin() + (i - 1));
            prev.data.erase(prev.data.begin() + (i - 1));
            UE_LOGI("floppybox: pop (eid=%u hash=%016llx) -> host", eid,
                    static_cast<unsigned long long>(h));
        }
        for (size_t i = p; i < cur.data.size() && !refused; ++i) {
            std::vector<uint8_t> b = Head(0, eid);
            const int32_t type = i < cur.types.size() ? cur.types[i] : 0;
            PutU32(b, static_cast<uint32_t>(type));
            PutStr(b, cur.data[i]);
            if (!coop::blob_chunks::SendBlob(s, coop::net::ReliableKind::FloppyBoxState,
                                             g_nextSeq++, b)) {
                refused = true;
                break;
            }
            prev.types.push_back(type);
            prev.data.push_back(cur.data[i]);
            UE_LOGI("floppybox: push (eid=%u type=%d) -> host", eid, type);
        }
        if (!refused) {
            it->second.digest = dig;  // fully handed over: SENT state complete
        }
        // refused: shadow reflects only the accepted ops; the digest stays
        // stale so the next sweep re-derives the remainder under fresh seqs.
    }
}

// DENY reap: the popper's spawned disc goes INTO THE HAND (measured getFloppy
// -> Hold Object). Reap-if-alive: destroy the held disc when it is disc-class
// within the correlation window; skip-if-consumed otherwise (R4 policy).
void ReapDeniedPop(uint32_t eid, uint64_t hash) {
    const uint64_t now = NowMs();
    bool windowed = false;
    for (const auto& t : g_takenRing)
        if (t.eid == eid && t.hash == hash && now - t.when < kDenyTtlMs) { windowed = true; break; }
    if (!windowed) {
        UE_LOGW("floppybox: DENY (eid=%u) outside the taken window -- nothing reaped", eid);
        return;
    }
    void* localPlayer = coop::players::Registry::Get().Local();
    ue_wrap::engine::MainPlayerGrabState gs{};
    if (localPlayer && ue_wrap::engine::ReadMainPlayerGrabState(localPlayer, gs)) {
        void* held = gs.grabbingActor ? gs.grabbingActor : gs.holdingActor;
        if (held && R::IsLive(held) &&
            ue_wrap::laptop::IsDiscClass(R::ClassOf(held))) {
            coop::prop_lifecycle::DestroyLocalProp(held, /*deferred*/true);
            UE_LOGW("floppybox: pop DENIED (eid=%u) -- held disc reaped", eid);
            return;
        }
    }
    UE_LOGI("floppybox: pop DENIED (eid=%u) -- disc already consumed/released, "
            "skip (content duplication is native-legal; canonical converges the box)", eid);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const uint64_t now = NowMs();
    if (!g_announced) {
        g_announced = true;
        UE_LOGI("floppybox: installed (LIFO stack lane)");
    }
    Sweep(s, now);  // self-gated 1 Hz; the assembler TTL sweep rides inside it (perf F2)
}

void OnBoxChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    std::vector<uint8_t> blob;
    if (!g_asm.OnChunk(p, senderSlot, blob)) return;
    if (blob.size() < 5) return;
    Reader r{blob.data(), blob.size()};
    const uint8_t op = r.U8();
    const uint32_t eid = r.U32();
    const bool host = IsHost();

    if (host) {
        void* actor = LivePropActor(eid);
        if (!actor || !FB::IsFloppyBoxClass(R::ClassOf(actor))) {
            UE_LOGW("floppybox: op=%u for unresolved box eid=%u -- dropped", op, eid);
            return;
        }
        FB::BoxArrays a;
        if (!FB::ReadArrays(actor, a)) return;
        if (op == 0) {  // push
            const int32_t type = static_cast<int32_t>(r.U32());
            const std::wstring str = r.Str();
            if (!r.ok) return;
            a.types.push_back(type);
            a.data.push_back(str);
            if (a.data.size() > 15)
                UE_LOGW("floppybox: box eid=%u exceeds the native cap (%zu) -- divergence "
                        "healed by canonical, watch for a dupe source", eid, a.data.size());
            FB::WriteArraysAndGen(actor, a);
            { FB::BoxArrays copy = a; PrimeShadow(eid, actor, std::move(copy)); }
            UE_LOGI("floppybox: push applied (eid=%u type=%d, from slot %u)",
                    eid, type, static_cast<unsigned>(senderSlot));
        } else if (op == 1) {  // pop: tail-anchored content search
            const uint64_t hash = r.U64();
            if (!r.ok) return;
            int32_t at = -1;
            for (size_t i = a.data.size(); i > 0; --i)
                if (HashStr(a.data[i - 1]) == hash) { at = static_cast<int32_t>(i - 1); break; }
            if (at < 0) {
                // DENY to the popper + canonical (heals its shadow).
                std::vector<uint8_t> b = Head(2, eid);
                PutU64(b, hash);
                coop::blob_chunks::SendBlobToSlot(s, senderSlot,
                                                  coop::net::ReliableKind::FloppyBoxState,
                                                  g_nextSeq++, b);
                UE_LOGW("floppybox: pop MISS (eid=%u hash=%016llx, slot %u) -- DENY sent",
                        eid, static_cast<unsigned long long>(hash),
                        static_cast<unsigned>(senderSlot));
            } else {
                a.types.erase(a.types.begin() + at);
                a.data.erase(a.data.begin() + at);
                FB::WriteArraysAndGen(actor, a);
                { FB::BoxArrays copy = a; PrimeShadow(eid, actor, std::move(copy)); }
                UE_LOGI("floppybox: pop applied (eid=%u idx=%d, from slot %u)",
                        eid, at, static_cast<unsigned>(senderSlot));
            }
        } else {
            return;  // clients never author deny/canonical
        }
        // R10-1 discipline: canonical after EVERY op (the ack).
        HostBroadcastCanonical(s, eid, actor);
        return;
    }

    // CLIENT: canonicals + denies, host-authored only.
    if (senderSlot != 0) {
        UE_LOGW("floppybox: op=%u from non-host slot %u -- REJECTED", op,
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (op == 2) {
        const uint64_t hash = r.U64();
        if (r.ok) ReapDeniedPop(eid, hash);
        return;
    }
    if (op != 3) return;
    FB::BoxArrays a;
    if (!ParseCanonical(r, a)) {
        UE_LOGW("floppybox: malformed canonical eid=%u -- dropped", eid);
        return;
    }
    void* actor = LivePropActor(eid);
    if (!actor || !FB::IsFloppyBoxClass(R::ClassOf(actor))) {
        PendingCanonical pc;
        pc.blob.assign(blob.begin() + 5, blob.end());
        pc.deadline = NowMs() + kPendingTtlMs;
        g_pending[eid] = std::move(pc);
        return;
    }
    ApplyCanonicalTo(actor, eid, a);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    std::vector<coop::element::Registry::ActorIdPair> pairs;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int shipped = 0;
    for (const auto& pr : pairs) {
        if (!pr.actor || !R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;
        if (!FB::IsFloppyBoxClass(R::ClassOf(pr.actor))) continue;
        FB::BoxArrays a;
        if (!FB::ReadArrays(pr.actor, a)) continue;
        const uint32_t beid = static_cast<uint32_t>(pr.id);
        if (coop::blob_chunks::SendBlobToSlot(s, peerSlot,
                                              coop::net::ReliableKind::FloppyBoxState,
                                              g_nextSeq++, PackCanonicalBounded(beid, a))) {
            ++shipped;
        } else {
            // CRIT-1: arm the broadcast retry (reaches the joiner too).
            g_canonRetry.insert(beid);
            UE_LOGW("floppybox: connect canonical eid=%u -> slot %d REFUSED -- "
                    "broadcast retry armed", beid, peerSlot);
        }
    }
    if (shipped)
        UE_LOGI("floppybox: connect canonicals -> slot %d (%d box(es))", peerSlot, shipped);
}

void OnDisconnect() {
    g_shadow.clear();
    g_pending.clear();
    for (auto& t : g_takenRing) t = Taken{};
    g_takenNext = 0;
    g_asm.Clear();
    g_nextSweep = 0;
    g_canonRetry.clear();
    g_announced = false;
    FB::ResetCache();
}

}  // namespace coop::floppybox_sync
