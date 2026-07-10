// coop/creatures/roach_sync.cpp -- see coop/creatures/roach_sync.h for the
// design + the bytecode ground truth. Shapes reused: serverbox_sync (lazy
// resolve + 1 Hz host poll + host-sender-checked client apply + connect
// replay), prop lanes (IsLiveByIndex liveness on a tracked set).

#include "coop/creatures/roach_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // kMaxPeers

#include "ue_wrap/call.h"
#include "ue_wrap/game_thread.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace coop::roach_sync {
namespace {

namespace R  = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr long long kPollIntervalMs     = 1000;   // host poll + client liveness scan
constexpr long long kKeepaliveMs        = 10000;  // periodic full resend (join/loss convergence)
constexpr float     kDriftResendDist    = 25.f;   // any roach moved this far -> resend
constexpr float     kApplyMoveEpsilon   = 1.f;    // skip SetWorldLocation under this delta
constexpr float     kApplyScaleEpsilon  = 0.05f;
constexpr float     kAdjudicateRadius   = 200.f;  // RoachConsumed nearest-match cap
constexpr int       kMaxIntentsPerTick  = 4;      // client consumption-intent send cap

// ---- resolution (lazy, 2 s retry, capped LOUD latch -- the serverbox pattern) ----
void*   g_gmCls        = nullptr;  // mainGamemode_C
int32_t g_offMaster    = -1;       // AcockroachMaster_C* cockroachMaster (0x910 in the dump)
void*   g_masterCls    = nullptr;  // cockroachMaster_C
int32_t g_offRoaches   = -1;       // TArray<UStaticMeshComponent*> roaches (0x238)
void*   g_addRoachFn   = nullptr;  // addRoach(FVector Location, FVector Size, bool bypassCheck)
void*   g_delRoachFn   = nullptr;  // deleteRoach(int32 IndexToRemove, bool crush)
void*   g_getLocFn     = nullptr;  // USceneComponent::K2_GetComponentLocation
void*   g_getScaleFn   = nullptr;  // USceneComponent::K2_GetComponentScale
void*   g_setLocFn     = nullptr;  // USceneComponent::K2_SetWorldLocation
void*   g_setScaleFn   = nullptr;  // USceneComponent::SetWorldScale3D
std::chrono::steady_clock::time_point g_nextResolve{};
int  g_postClassAttempts = 0;
bool g_resolveLatched = false;
constexpr int kMaxPostClassAttempts = 5;

struct Vec3 { float x, y, z; };

bool Resolved() {
    return g_offMaster >= 0 && g_offRoaches >= 0 && g_addRoachFn && g_delRoachFn &&
           g_getLocFn && g_getScaleFn && g_setLocFn && g_setScaleFn;
}

void ResolvePass() {
    if (g_resolveLatched) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);
    if (!g_gmCls)     g_gmCls     = R::FindClass(L"mainGamemode_C");
    if (!g_masterCls) g_masterCls = R::FindClass(L"cockroachMaster_C");
    if (!g_gmCls || !g_masterCls) return;  // world not loaded yet
    if (g_offMaster  < 0) g_offMaster  = R::FindPropertyOffset(g_gmCls, L"cockroachMaster");
    if (g_offRoaches < 0) g_offRoaches = R::FindPropertyOffset(g_masterCls, L"roaches");
    if (!g_addRoachFn) g_addRoachFn = R::FindFunction(g_masterCls, L"addRoach");
    if (!g_delRoachFn) g_delRoachFn = R::FindFunction(g_masterCls, L"deleteRoach");
    if (void* sceneCls = R::FindClass(L"SceneComponent")) {
        if (!g_getLocFn)   g_getLocFn   = R::FindFunction(sceneCls, L"K2_GetComponentLocation");
        if (!g_getScaleFn) g_getScaleFn = R::FindFunction(sceneCls, L"K2_GetComponentScale");
        if (!g_setLocFn)   g_setLocFn   = R::FindFunction(sceneCls, L"K2_SetWorldLocation");
        if (!g_setScaleFn) g_setScaleFn = R::FindFunction(sceneCls, L"SetWorldScale3D");
    }
    if (Resolved()) {
        g_resolveLatched = true;
        UE_LOGI("roach_sync: resolved (master@0x%X roaches@0x%X addRoach=%p deleteRoach=%p "
                "loc/scale get+set=ok)", g_offMaster, g_offRoaches, g_addRoachFn, g_delRoachFn);
        return;
    }
    if (++g_postClassAttempts >= kMaxPostClassAttempts) {
        g_resolveLatched = true;
        UE_LOGW("roach_sync: resolution INCOMPLETE after %d passes (master@0x%X roaches@0x%X "
                "addRoach=%s deleteRoach=%s) -- latched OFF; game version mismatch?",
                g_postClassAttempts, g_offMaster, g_offRoaches,
                g_addRoachFn ? "yes" : "no", g_delRoachFn ? "yes" : "no");
    }
}

// ---- live gamemode + master (cached, revalidated by internal index) ----
void*   g_gm = nullptr;
int32_t g_gmIdx = -1;

void* Gamemode() {
    if (!g_gm || !R::IsLiveByIndex(g_gm, g_gmIdx)) {
        g_gm = R::FindObjectByClass(L"mainGamemode_C");
        g_gmIdx = g_gm ? R::InternalIndexOf(g_gm) : -1;
    }
    return g_gm;
}

void* Master() {
    void* gm = Gamemode();
    if (!gm || g_offMaster < 0) return nullptr;
    void* m = *reinterpret_cast<void* const*>(reinterpret_cast<uint8_t*>(gm) + g_offMaster);
    if (!m || !R::IsLive(m)) return nullptr;
    return m;
}

struct FArrayRaw { void* Data; int32_t Num; int32_t Max; };

// Valid (non-null, live) roach components in array order, with their RAW index
// (deleteRoach takes the raw index; the wire carries the ORDINAL order).
struct RoachSlot { void* comp; int32_t rawIdx; };

void ReadRoaches(void* master, std::vector<RoachSlot>& out) {
    out.clear();
    if (!master || g_offRoaches < 0) return;
    const FArrayRaw* arr = reinterpret_cast<const FArrayRaw*>(
        reinterpret_cast<uint8_t*>(master) + g_offRoaches);
    if (!arr->Data || arr->Num <= 0) return;
    void* const* elems = reinterpret_cast<void* const*>(arr->Data);
    for (int32_t i = 0; i < arr->Num && out.size() < coop::net::kRoachSnapshotCap; ++i) {
        void* c = elems[i];
        if (c && R::IsLive(c)) out.push_back({c, i});
    }
}

bool GetCompLocation(void* comp, Vec3& out) {
    ue_wrap::ParamFrame f(g_getLocFn);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

bool GetCompScale(void* comp, Vec3& out) {
    ue_wrap::ParamFrame f(g_getScaleFn);
    if (!f.valid() || !ue_wrap::Call(comp, f)) return false;
    return f.GetRaw(L"ReturnValue", &out, sizeof(out));
}

void SetCompLocation(void* comp, const Vec3& v) {
    ue_wrap::ParamFrame f(g_setLocFn);
    if (!f.valid()) return;
    f.SetRaw(L"NewLocation", &v, sizeof(v));
    f.Set<bool>(L"bSweep", false);
    f.Set<bool>(L"bTeleport", true);
    ue_wrap::Call(comp, f);
}

void SetCompScale(void* comp, float s) {
    Vec3 v{s, s, s};
    ue_wrap::ParamFrame f(g_setScaleFn);
    if (!f.valid()) return;
    f.SetRaw(L"NewScale3D", &v, sizeof(v));
    ue_wrap::Call(comp, f);
}

float Dist2(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

long long g_lastPollMs = 0;

// ---- HOST: snapshot + change detection + paged send ----
struct SnapEntry { Vec3 loc; float scale; };
std::vector<SnapEntry> g_lastSent;    // baseline for drift detection
uint32_t  g_seq = 0;
long long g_lastSendMs = 0;
void*     g_polledMaster = nullptr;   // re-prime on world/save reload

bool ReadSnapshot(void* master, std::vector<SnapEntry>& out) {
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    out.clear();
    out.reserve(slots.size());
    for (const auto& s : slots) {
        SnapEntry e{};
        if (!GetCompLocation(s.comp, e.loc)) continue;
        Vec3 sc{1.f, 1.f, 1.f};
        GetCompScale(s.comp, sc);
        e.scale = sc.x;  // uniform (the game scales roaches uniformly)
        out.push_back(e);
    }
    return true;
}

bool SnapshotChanged(const std::vector<SnapEntry>& cur) {
    if (cur.size() != g_lastSent.size()) return true;
    const float d2 = kDriftResendDist * kDriftResendDist;
    for (size_t i = 0; i < cur.size(); ++i) {
        if (Dist2(cur[i].loc, g_lastSent[i].loc) > d2) return true;
        if (std::fabs(cur[i].scale - g_lastSent[i].scale) > kApplyScaleEpsilon) return true;
    }
    return false;
}

// Send the full snapshot as pages; toSlot < 0 broadcasts to every client.
void SendSnapshot(coop::net::Session* s, const std::vector<SnapEntry>& snap, int toSlot) {
    ++g_seq;
    const int total = static_cast<int>(snap.size());
    const int pages = total == 0 ? 1
                                 : (total + coop::net::kRoachEntriesPerPage - 1) /
                                       coop::net::kRoachEntriesPerPage;
    for (int pg = 0; pg < pages; ++pg) {
        coop::net::RoachStatePayload p{};
        p.seq        = g_seq;
        p.page       = static_cast<uint8_t>(pg);
        p.pageCount  = static_cast<uint8_t>(pages);
        p.totalCount = static_cast<uint8_t>(total);
        const int base = pg * coop::net::kRoachEntriesPerPage;
        int n = 0;
        for (; n < coop::net::kRoachEntriesPerPage && base + n < total; ++n) {
            const SnapEntry& e = snap[base + n];
            p.entries[n] = {e.loc.x, e.loc.y, e.loc.z, e.scale};
        }
        p.entryCount = static_cast<uint8_t>(n);
        const bool ok = toSlot < 0
            ? s->SendReliable(coop::net::ReliableKind::RoachState, &p, sizeof(p))
            : s->SendReliableToSlot(toSlot, coop::net::ReliableKind::RoachState, &p, sizeof(p));
        if (!ok) {
            UE_LOGW("roach_sync: RoachState send FAILED (seq=%u page %d/%d, toSlot=%d)",
                    g_seq, pg + 1, pages, toSlot);
            return;  // partial snapshot: receiver's seq/page assembly discards it
        }
    }
}

// ---- CLIENT: page assembly + ordinal apply + local-consumption tracking ----
uint32_t g_asmSeq = 0;
uint16_t g_asmPagesGot = 0;      // bitmask (pageCount <= 11 for 128 roaches)
uint8_t  g_asmPageCount = 0;
uint8_t  g_asmTotal = 0;
SnapEntry g_asmEntries[coop::net::kRoachSnapshotCap];

// The tracked mirror set: component + internal index + last applied position.
// Liveness on THIS set is the local-consumption detector (a native eat/stomp
// K2_DestroyComponent's the entry -- no polling dispatches needed).
struct TrackedRoach { void* comp; int32_t idx; Vec3 loc; };
std::vector<TrackedRoach> g_tracked;
bool  g_haveApplied = false;
void* g_trackedMaster = nullptr;  // the master the tracked set belongs to (teardown guard)

void RebuildTracked(void* master) {
    g_tracked.clear();
    g_trackedMaster = master;
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    for (const auto& s : slots) {
        Vec3 loc{};
        if (!GetCompLocation(s.comp, loc)) continue;
        g_tracked.push_back({s.comp, R::InternalIndexOf(s.comp), loc});
    }
}

void ApplySnapshot(const SnapEntry* entries, int total) {
    void* master = Master();
    if (!master) return;  // no world/master yet; the next snapshot re-delivers
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);

    if (static_cast<int>(slots.size()) == total) {
        // Ordinal drive: k-th local valid slot <- k-th snapshot entry.
        for (int i = 0; i < total; ++i) {
            Vec3 cur{};
            const Vec3 want{entries[i].loc.x, entries[i].loc.y, entries[i].loc.z};
            if (GetCompLocation(slots[i].comp, cur) &&
                Dist2(cur, want) > kApplyMoveEpsilon * kApplyMoveEpsilon) {
                SetCompLocation(slots[i].comp, want);
            }
            Vec3 sc{1.f, 1.f, 1.f};
            if (GetCompScale(slots[i].comp, sc) &&
                std::fabs(sc.x - entries[i].scale) > kApplyScaleEpsilon) {
                SetCompScale(slots[i].comp, entries[i].scale);
            }
        }
    } else {
        // Population rebuild via the game's OWN mutators (array/Count/collision
        // stay SP-consistent). deleteRoach nulls the slot (no index shift), so
        // deleting in array order is safe; addRoach appends.
        for (const auto& s : slots) {
            ue_wrap::ParamFrame del(g_delRoachFn);
            if (!del.valid()) break;
            del.Set<int32_t>(L"IndexToRemove", s.rawIdx);
            del.Set<bool>(L"crush", false);
            ue_wrap::Call(master, del);
        }
        for (int i = 0; i < total; ++i) {
            ue_wrap::ParamFrame add(g_addRoachFn);
            if (!add.valid()) break;
            const Vec3 loc{entries[i].loc.x, entries[i].loc.y, entries[i].loc.z};
            const Vec3 size{entries[i].scale, entries[i].scale, entries[i].scale};
            add.SetRaw(L"Location", &loc, sizeof(loc));
            add.SetRaw(L"Size", &size, sizeof(size));
            add.Set<bool>(L"bypassCheck", true);
            ue_wrap::Call(master, add);
        }
        UE_LOGI("roach_sync: client REBUILT population (%zu local -> %d host roaches)",
                slots.size(), total);
    }
    RebuildTracked(master);
    g_haveApplied = true;
}

// Client 1 Hz: any tracked component that died locally (and not via our own
// apply, which rebuilds g_tracked) was consumed by the LOCAL player's native
// eat/stomp event -> forward the intent.
void DetectLocalConsumption(coop::net::Session* s) {
    if (!g_haveApplied || g_tracked.empty()) return;
    // Master-liveness gate (audit 2026-07-10 CRITICAL): a client WORLD TEARDOWN
    // while the session is still connected (idle death -> menu -- observed on
    // both peers 07-10) kills every tracked component at once; without this
    // gate that mass-death would stream false RoachConsumed intents at
    // host-correct positions and WIPE the host's live population. A dead or
    // REPLACED master means the deaths were structural, not consumption.
    void* master = Master();
    if (!master || master != g_trackedMaster) {
        g_tracked.clear();
        g_haveApplied = false;
        return;
    }
    int sent = 0;
    size_t w = 0;
    for (size_t r = 0; r < g_tracked.size(); ++r) {
        TrackedRoach& t = g_tracked[r];
        if (R::IsLiveByIndex(t.comp, t.idx)) { g_tracked[w++] = t; continue; }
        if (sent < kMaxIntentsPerTick) {
            coop::net::RoachConsumedPayload p{t.loc.x, t.loc.y, t.loc.z};
            if (s->SendReliable(coop::net::ReliableKind::RoachConsumed, &p, sizeof(p))) {
                ++sent;
                UE_LOGI("roach_sync: local roach consumed at (%.0f,%.0f,%.0f) -> intent to host",
                        t.loc.x, t.loc.y, t.loc.z);
            } else {
                g_tracked[w++] = t;  // send failed -> keep, retry next tick
            }
        } else {
            g_tracked[w++] = t;      // over the per-tick cap -> retry next tick
        }
    }
    g_tracked.resize(w);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!GT::IsGameThread()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    const long long now = NowMs();
    if (now - g_lastPollMs < kPollIntervalMs) return;
    g_lastPollMs = now;
    ResolvePass();
    if (!Resolved()) return;

    if (s->role() != coop::net::Role::Host) {
        DetectLocalConsumption(s);
        return;
    }

    // HOST: poll -> paged broadcast on change / drift / keepalive.
    void* master = Master();
    if (!master) return;
    std::vector<SnapEntry> snap;
    ReadSnapshot(master, snap);
    if (master != g_polledMaster) {
        // World/save reload minted a new master -- re-prime silently (the
        // serverbox re-prime rule: a prime never masquerades as an edge).
        g_polledMaster = master;
        g_lastSent = snap;
        g_lastSendMs = now;
        return;
    }
    const bool changed   = SnapshotChanged(snap);
    const bool keepalive = (now - g_lastSendMs >= kKeepaliveMs) &&
                           (!snap.empty() || !g_lastSent.empty());
    if (!changed && !keepalive) return;
    g_lastSent = snap;
    g_lastSendMs = now;
    SendSnapshot(s, snap, -1);
    if (changed)
        UE_LOGI("roach_sync: host broadcast %zu roach(es) (seq=%u)", snap.size(), g_seq);
}

void OnState(const coop::net::RoachStatePayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) { UE_LOGW("roach_sync: OnState off-game-thread -- dropping"); return; }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() == coop::net::Role::Host) {
        UE_LOGW("roach_sync: RoachState received on the HOST -- dropping from slot=%d", senderPeerSlot);
        return;
    }
    if (senderPeerSlot != 0) {
        UE_LOGW("roach_sync: RoachState from non-host senderPeerSlot=%d -- dropping", senderPeerSlot);
        return;
    }
    ResolvePass();
    if (!Resolved()) return;  // the keepalive re-delivers post-resolve
    if (payload.pageCount == 0 || payload.page >= payload.pageCount ||
        payload.pageCount > (coop::net::kRoachSnapshotCap + coop::net::kRoachEntriesPerPage - 1) /
                                coop::net::kRoachEntriesPerPage ||
        payload.totalCount > coop::net::kRoachSnapshotCap ||
        payload.entryCount > coop::net::kRoachEntriesPerPage) {  // entries[] is Entry[12] -- OOB read otherwise
        UE_LOGW("roach_sync: malformed RoachState (page %u/%u entries=%u total=%u) -- dropping",
                payload.page, payload.pageCount, payload.entryCount, payload.totalCount);
        return;
    }
    if (payload.seq != g_asmSeq) {  // new snapshot -> restart assembly
        g_asmSeq = payload.seq;
        g_asmPagesGot = 0;
        g_asmPageCount = payload.pageCount;
        g_asmTotal = payload.totalCount;
    }
    const int base = payload.page * coop::net::kRoachEntriesPerPage;
    for (int i = 0; i < payload.entryCount && base + i < coop::net::kRoachSnapshotCap; ++i) {
        const auto& e = payload.entries[i];
        g_asmEntries[base + i] = SnapEntry{{e.x, e.y, e.z}, e.scale};
    }
    g_asmPagesGot |= static_cast<uint16_t>(1u << payload.page);
    const uint16_t want = static_cast<uint16_t>((1u << g_asmPageCount) - 1u);
    if ((g_asmPagesGot & want) != want) return;  // more pages coming (in-order lane)
    ApplySnapshot(g_asmEntries, g_asmTotal);
}

void OnConsumedIntent(const coop::net::RoachConsumedPayload& payload, int senderPeerSlot) {
    if (!GT::IsGameThread()) { UE_LOGW("roach_sync: OnConsumedIntent off-game-thread -- dropping"); return; }
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    if (senderPeerSlot <= 0) {
        UE_LOGW("roach_sync: RoachConsumed from slot=%d (not a client) -- dropping", senderPeerSlot);
        return;
    }
    ResolvePass();
    if (!Resolved()) return;
    void* master = Master();
    if (!master) return;
    std::vector<RoachSlot> slots;
    ReadRoaches(master, slots);
    const Vec3 at{payload.x, payload.y, payload.z};
    int32_t bestIdx = -1;
    float bestD2 = kAdjudicateRadius * kAdjudicateRadius;
    for (const auto& slot : slots) {
        Vec3 loc{};
        if (!GetCompLocation(slot.comp, loc)) continue;
        const float d2 = Dist2(loc, at);
        if (d2 <= bestD2) { bestD2 = d2; bestIdx = slot.rawIdx; }
    }
    if (bestIdx < 0) {
        // Already gone (host trim raced the intent) or a stale/duplicate
        // intent -- benign; the periodic snapshot has converged everyone.
        UE_LOGI("roach_sync: consumed-intent from slot %d matched no roach within %.0f -- no-op",
                senderPeerSlot, kAdjudicateRadius);
        return;
    }
    ue_wrap::ParamFrame del(g_delRoachFn);
    if (!del.valid()) return;
    del.Set<int32_t>(L"IndexToRemove", bestIdx);
    del.Set<bool>(L"crush", false);
    ue_wrap::Call(master, del);
    UE_LOGI("roach_sync: slot %d consumed roach rawIdx=%d (d=%.0f) -- deleted; next poll broadcasts",
            senderPeerSlot, bestIdx, std::sqrt(bestD2));
}

void QueueConnectBroadcastForSlot(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != coop::net::Role::Host) return;
    if (slot < 0 || slot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!Resolved()) return;  // no world yet -> the keepalive delivers state
    void* master = Master();
    if (!master) return;
    std::vector<SnapEntry> snap;
    ReadSnapshot(master, snap);
    if (snap.empty()) return;  // joiner starts at zero roaches natively
    SendSnapshot(s, snap, slot);
    UE_LOGI("roach_sync: connect-snapshot -- %zu roach(es) to slot %d", snap.size(), slot);
}

void OnDisconnect() {
    g_gm = nullptr; g_gmIdx = -1;
    g_polledMaster = nullptr;
    g_lastSent.clear();
    g_lastSendMs = 0; g_lastPollMs = 0;
    g_asmSeq = 0; g_asmPagesGot = 0; g_asmPageCount = 0; g_asmTotal = 0;
    g_tracked.clear();
    g_trackedMaster = nullptr;
    g_haveApplied = false;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::roach_sync
