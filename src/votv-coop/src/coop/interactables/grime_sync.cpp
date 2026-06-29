// coop/grime_sync.cpp -- see coop/grime_sync.h. Surface grime dirt sync.
//
// A sibling of window_sync (the keyed monotone-min-wins scalar channel) keyed by a QUANTIZED
// WORLD-POSITION string instead of an FName Key: Agrime_C is a STATIC level-placed decal, so its
// saved transform is identical across peers (same save) and its position is its cross-peer
// identity. The one engine difference from the window: the apply repaints via Agrime_C::
// applyMaterial (not a pure setter). A GRADUAL wipe is caught by the normal process poll (driven to
// invisible); a ONE-SHOT / SUPER-SPONGE wipe (process past 0 in a single hit -> the actor self-
// destructs before the poll sees the low value) is caught by a PROXIMITY-GATED death-watch in
// PollAndBroadcast -- a decal that vanishes NEAR the local camera was wiped (broadcast value=0); FAR
// = a sublevel stream-out, ignored (the ungated version flooded false destroys on the connect
// stream-out -- the first smoke caught it). See grime_sync.h. (K2_DestroyActor PRE is NOT usable: the
// BP-internal clean()->K2_DestroyActor bypasses the ProcessEvent detour, like the trash-clump morph.)
//
// (window_sync + grime_sync are now two near-identical keyed-float channels; if a third appears,
// generalize into a shared Adapter+Channel the way interactable_sync did for its 3 bool features.)

#include "coop/interactables/grime_sync.h"

#include "coop/session/ini_config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/engine.h"          // GetActorLocation (the grime's world position -> posKey)
#include "ue_wrap/grime.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "coop/util/incremental_object_scan.h"  // L5: scan only NEW objects (no 237k walk)
#include "ue_wrap/walk_timer.h"           // L5: [WALK-TIME] profiling

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>          // swscanf (decode a PosKey back to a world position for the proximity check)
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::grime_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace G = ue_wrap::grime;
namespace E = ue_wrap::engine;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
// Poll throttle. Unlike the window (a handful of instances), grime has ~1000 decals, so polling
// every frame is wasteful. A sponge wipe lasts ~1-2 s (multiple hits), so 20 Hz still captures the
// process gradient finely while cutting the per-poll cost ~3-6x.
constexpr auto kPollThrottle = std::chrono::milliseconds(50);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// process changes in discrete wipe steps; this epsilon only filters float-equality noise.
constexpr float kProcessEps = 0.0005f;
// Position quantization grid (cm). A grime decal's saved position is bit-identical across peers
// (same save, static actor), so any deterministic quantization yields the same key on both; the
// grid just has to be fine enough that two DISTINCT decals never share a cell. 2 cm + the Type
// disambiguator is ample (decals are not stacked sub-2-cm).
constexpr double kPosGrid = 2.0;
// Death-watch proximity radius (cm). A grime decal that VANISHES from the index was either WIPED
// (the local player one-shot-cleaned it with a sponge -- esp. the max-strength SUPER SPONGE, which
// drives process past 0 in a single hit the 20 Hz poll never captures) or STREAMED OUT (its sublevel
// unloaded as the player moved -- a connect-teleport unloads hundreds at once). They are told apart
// by PROXIMITY to the local camera: you can only sponge a decal you are standing AT (a wipe is within
// a few metres of the view), whereas a streamed-out sublevel is far away (>10 m). A vanish within
// this radius is treated as a WIPE -> broadcast value=0 (drive the peer's mirror to clean); farther
// vanishes are ignored (the naive ungated death-watch flooded false destroys on the connect stream-out).
// 800 cm (8 m): generous vs any sponge reach (the sponge cleans by CONTACT -- ComponentHit +
// SphereOverlapActors(cleanRadius) -- so the cleaned decal is ~arm's length + cleanRadius from the
// view, ~2-3 m; the extra margin covers a longer-reach super sponge) yet far below the connect-
// teleport stream-out distance (~14 m, smoke-measured) so the stream-out is still cleanly excluded.
// Erring slightly LOOSE is deliberate: a MISS silently breaks the sync for that wipe, whereas a rare
// false-fire only MIN-applies value=0 (a recoverable spurious clean).
constexpr float kWipeProximityCm  = 800.0f;
constexpr float kWipeProximityCm2 = kWipeProximityCm * kWipeProximityCm;

bool ProbeLog() {
    static const bool s_enabled = ::coop::ini_config::IsIniKeyTrue("grime_log");
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_echo{false};  // belt-and-suspenders: suppress the poll mid-apply (GT-serial anyway)

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

std::mutex g_stateMutex;
std::unordered_map<std::wstring, float> g_lastKnown;  // key -> last broadcast/applied process

struct Pending { float value; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // GT-only: deferred GrimeState applies

// GT-only: posKeys whose decal WE wiped to destruction (the death-watch fired -> we sent value=0).
// The HOST re-sends value=0 for these in the connect-snapshot so a joiner cleans a decal that was
// wiped BEFORE it joined (a destroyed decal is gone from the index, so the normal snapshot misses it).
// Bounded by the world's decal count.
std::unordered_set<std::wstring> g_wipedKeys;

std::chrono::steady_clock::time_point g_lastRetry{};  // GT-only: rebuild + deferred-retry throttle
std::chrono::steady_clock::time_point g_lastPoll{};   // GT-only: process-poll throttle
size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only
std::vector<std::pair<std::wstring, float>> g_sendScratch;  // GT-only: reused buffer for the RARE wipes to broadcast (usually empty -> no per-poll alloc)

// The cross-peer identity of a STATIC grime decal: its quantized world position + Type. Same save
// -> identical saved transform -> identical key on both peers. Fits a 31-char WireKey for any
// in-base coordinate (|coord| < ~300 km would overflow; VOTV's base is ~+/-40 km cm).
std::wstring PosKey(void* grime) {
    const ue_wrap::FVector loc = E::GetActorLocation(grime);
    int32_t type = 0; G::ReadType(grime, type);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    std::wstring k = L"g_";
    k += std::to_wstring(q(loc.X)); k += L'_';
    k += std::to_wstring(q(loc.Y)); k += L'_';
    k += std::to_wstring(q(loc.Z)); k += L'_';
    k += std::to_wstring(type);
    return k;
}

// Recover a decal's approximate world position from its PosKey (the inverse of PosKey's quantization).
// Used only by the death-watch proximity check, where the 2 cm grid error is negligible vs the ~5 m
// radius. Returns false if the key is malformed.
bool DecodePosKey(const std::wstring& key, ue_wrap::FVector& out) {
    int gx = 0, gy = 0, gz = 0, type = 0;
    if (std::swscanf(key.c_str(), L"g_%d_%d_%d_%d", &gx, &gy, &gz, &type) < 3) return false;
    out.X = static_cast<float>(gx * kPosGrid);
    out.Y = static_cast<float>(gy * kPosGrid);
    out.Z = static_cast<float>(gz * kPosGrid);
    return true;
}

void* ResolveFast(const std::wstring& key) {
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
        return it->second.actor;
    return nullptr;
}

// Full GUObjectArray walk -> rebuild the position->actor index. Game thread, throttled (NOT
// per-frame). Logs a (count, posHash) that host vs client compare for cross-peer position
// stability -- the grime analog of the window's keysHash (if these MATCH, the position-identity
// model is proven; if not, the decals diverge across peers and the model needs revisiting).
size_t RebuildIndex() {
    if (!G::EnsureResolved()) return 0;
    // PosKey cache (perf audit H-1): a grime decal is STATIC, so its PosKey never changes -- yet
    // computing it dispatches GetActorLocation (a ProcessEvent UFunction), and doing that for ~1000
    // unchanged decals on every 2 s rebuild was a recurring ~3 ms frame hitch. Cache PosKey per actor
    // pointer: only a NEW (streamed-in) decal computes it; known decals reuse the cached string. The
    // cache is rebuilt from the live set each pass (a re-streamed decal gets a fresh pointer ->
    // recomputes; dead actors drop). GT-only (RebuildIndex is game-thread-serial). [Ideal per the
    // audit is a direct RootComponent->location field read with no UFunction at all -- queued.]
    static std::unordered_map<void*, std::wstring> s_posKeyByActor;
    // L5: scan only the range NextRange gives -- the array TAIL (new decals) every call, a FULL re-scan
    // every ~5min (the slot-reuse backstop). A grime decal is a level-loaded Agrime_C, so the tail-scan
    // catches it at stream-in; the rare full safety covers any slot-reuse drift. The PosKey cache below
    // is maintained incrementally (new actors compute + cache; the full scan rebuilds it from scratch).
    static coop::util::IncrementalObjectScan sScan;
    const auto r = coop::util::NextRange(sScan);
    std::unordered_map<void*, std::wstring> nextCache;  // populated only on a full scan
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(64);
    for (int32_t i = r.begin; i < r.end; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!G::IsGrime(obj)) continue;  // cheap class-descendant filter (no alloc)
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        auto cit = s_posKeyByActor.find(obj);
        std::wstring key = (cit != s_posKeyByActor.end()) ? cit->second : PosKey(obj);
        if (r.isFull) nextCache.emplace(obj, key);   // full scan rebuilds the cache from the live set
        else          s_posKeyByActor.emplace(obj, key);  // tail scan: cache the NEW actor's key
        found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }
    if (r.isFull) s_posKeyByActor.swap(nextCache);  // keep live actors' cached keys, drop dead ones
    uint64_t posHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (r.isFull) g_byKey.clear();                         // full re-scan: rebuild from scratch
        for (auto& f : found) g_byKey[f.first] = f.second;     // add the new (or all, on a full scan)
        if (!r.isFull) {                                       // tail scan: prune dead entries (cheap, O(index))
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else { s_posKeyByActor.erase(it->second.actor); it = g_byKey.erase(it); }
            }
        }
        for (auto& kv : g_byKey) posHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
    }
    if (total != g_lastLogCount || posHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = posHash;
        UE_LOGI("grime: index rebuilt -- %zu live grime decal(s), posHash=0x%016llX (%s scan, +%zu new) "
                "(compare host vs client for cross-peer position stability)",
                total, static_cast<unsigned long long>(posHash), r.isFull ? "full" : "tail", found.size());
    }
    if (ProbeLog())
        for (auto& f : found)
            UE_LOGI("grime[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
    return total;
}

// Apply a remote process value. adopt -> VERBATIM (host connect-snapshot); else MIN(local, wire)
// (a live wipe can only clean, never re-dirty). Idempotent if already at target. Primes
// g_lastKnown so the next poll sees no delta (echo guard).
void ApplyResolved(void* actor, const std::wstring& key, float wireProcess, bool adopt, unsigned fromSlot) {
    float cur = 0.f;
    if (!G::ReadProcess(actor, cur)) return;
    const float target = adopt ? wireProcess : std::min(cur, wireProcess);
    if (std::fabs(target - cur) < kProcessEps) {
        std::lock_guard<std::mutex> lk(g_stateMutex);
        g_lastKnown[key] = target;  // converge the poll baseline; no write
        if (ProbeLog())
            UE_LOGI("grime: apply key='%ls' already %.3f -- idempotent skip", key.c_str(), target);
        return;
    }
    g_echo.store(true, std::memory_order_release);
    const bool ok = G::WriteProcessAndApply(actor, target);
    g_echo.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[key] = target; }
    UE_LOGI("grime: applied process=%.3f (wire=%.3f adopt=%d) ok=%d key='%ls' (from slot %u)",
            target, wireProcess, adopt ? 1 : 0, ok ? 1 : 0, key.c_str(), fromSlot);
}

// SENDER: poll every indexed grime for a process DECREASE (broadcast a wipe) + DEATH-WATCH (an
// indexed grime that is no longer live self-destructed from a wipe-below-0 -> broadcast destroy).
// Game thread.
void PollAndBroadcast() {
    if (g_echo.load(std::memory_order_acquire)) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    // Collect the (RARE) wipes to broadcast AFTER releasing the locks. We iterate the ~1000-entry
    // index IN PLACE rather than snapshotting every key into a scratch vector each poll: the ~20-char
    // position keys exceed the SSO limit, so a per-poll snapshot would heap-thrash ~1000 allocs.
    // A wipe is a rare user action, so g_sendScratch is empty on virtually every poll (no alloc).
    // The locks are GT-serial with RebuildIndex (no real contention -- grime has no observer); a
    // direct field read (ReadProcess) + reflection array check (IsLiveByIndex) are safe under them.
    auto& toSend = g_sendScratch;
    toSend.clear();
    std::vector<std::wstring> vanished;  // decals gone from the index this poll (wiped OR streamed out); empty in steady state
    {
        std::lock_guard<std::mutex> lkI(g_indexMutex);
        if (g_byKey.empty()) return;
        std::lock_guard<std::mutex> lkS(g_stateMutex);
        for (auto& kv : g_byKey) {
            if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx)) { vanished.push_back(kv.first); continue; }
            float cur = 0.f;
            if (!G::ReadProcess(kv.second.actor, cur)) continue;
            auto it = g_lastKnown.find(kv.first);
            if (it == g_lastKnown.end()) { g_lastKnown.emplace(kv.first, cur); continue; }  // prime silently
            if (cur < it->second - kProcessEps) { toSend.emplace_back(kv.first, cur); it->second = cur; }  // a (partial) wipe
            else if (cur > it->second + kProcessEps) { it->second = cur; }  // got dirtier (save reload) -- resync silently, never propagate a re-dirty
        }
    }
    // A VANISHED decal was WIPED (a one-shot self-destruct at process<0 -- the SUPER-SPONGE case the
    // poll can't see, since the actor is gone before the next poll caught the low process) or STREAMED
    // OUT (its sublevel unloaded). Tell them apart by PROXIMITY to the local camera: a wipe happens AT
    // the player (you sponge a decal you stand at); a streamed-out sublevel is far. For a NEAR (wiped)
    // decal broadcast value=0 -> the peer's mirror MIN-applies 0 -> fully clean (invisible); FAR
    // vanishes are ignored (the ungated death-watch flooded false destroys on the connect stream-out).
    // The camera UFunction is read ONCE, only when something vanished. Then drop every vanished decal
    // from the index (a stream-back re-adds it on the next rebuild).
    if (!vanished.empty()) {
        const ue_wrap::FVector cam = E::GetCameraLocation();
        for (const auto& key : vanished) {
            ue_wrap::FVector gp;
            if (!DecodePosKey(key, gp)) continue;
            const float dx = cam.X - gp.X, dy = cam.Y - gp.Y, dz = cam.Z - gp.Z;
            if (dx * dx + dy * dy + dz * dz > kWipeProximityCm2) continue;  // far -> stream-out, not a wipe
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(key, p.key);
            p.value = 0.f;  // wiped to destruction -> drive the mirror fully clean (MIN-applied -> invisible)
            p.adopt = 0;
            if (s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p))) {
                g_wipedKeys.insert(key);  // remember for the connect-snapshot (a joiner cleans pre-join wipes)
                UE_LOGI("grime: sent WIPE (value=0) key='%ls'", key.c_str());
            }
        }
        std::lock_guard<std::mutex> lkI(g_indexMutex);
        std::lock_guard<std::mutex> lkS(g_stateMutex);
        for (const auto& key : vanished) { g_byKey.erase(key); g_lastKnown.erase(key); }
    }
    for (auto& t : toSend) {
        coop::net::KeyedScalarPayload p{};
        WireKeyFromString(t.first, p.key);
        p.value = t.second;
        p.adopt = 0;  // live wipe -> receivers apply MIN
        if (s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p)))
            UE_LOGI("grime: sent process=%.3f key='%ls'", t.second, t.first.c_str());
        else
            UE_LOGW("grime: SendReliable failed key='%ls'", t.first.c_str());
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (G::EnsureResolved()) {
        static bool s_indexed = false;
        if (!s_indexed) { UE_LOGI("grime: indexed %zu decal(s)", RebuildIndex()); s_indexed = true; }
    }
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("grime: OnReliable empty key -- dropping"); return; }
    if (!G::EnsureResolved()) {
        UE_LOGW("grime: apply -- class not resolved, dropping key='%ls'", key.c_str());
        return;
    }
    // GrimeState. Trust boundary (same as the window): adopt==1 (VERBATIM, can re-dirty) is honored
    // ONLY from the host (slot 0); a client edge is forced to a min-wins live wipe.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    if (void* actor = ResolveFast(key)) { ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot); return; }
    // Not streamed in yet -- defer + retry (merge: adopt overrides verbatim, live keeps the MIN).
    const auto deadline = std::chrono::steady_clock::now() + kPendingTTL;
    auto it = g_pending.find(key);
    if (it == g_pending.end()) {
        g_pending[key] = Pending{ payload.value, adopt, senderPeerSlot, deadline };
    } else {
        if (adopt) { it->second.value = payload.value; it->second.adopt = true; }
        else       { it->second.value = std::min(it->second.value, payload.value); }
        it->second.fromSlot = senderPeerSlot;
        it->second.deadline = deadline;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    RebuildIndex();
    // Iterate the ~1000-entry index IN PLACE (no per-key wstring snapshot -- same reason as
    // PollAndBroadcast). The connect edge is game-thread-serial with RebuildIndex (no observer ->
    // no real contention), so holding both locks across the SendReliableToSlot enqueues is safe.
    int sent = 0;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lkI(g_indexMutex);
        std::lock_guard<std::mutex> lkS(g_stateMutex);
        total = g_byKey.size();
        for (auto& kv : g_byKey) {
            if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx)) continue;
            float process = 0.f;
            if (!G::ReadProcess(kv.second.actor, process)) continue;
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(kv.first, p.key);
            p.value = process;
            p.adopt = 1;  // connect-snapshot -> the joiner adopts the host's world VERBATIM
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            g_lastKnown[kv.first] = process;
            ++sent;
        }
    }
    // Also clean every decal we WIPED to destruction this session: it is gone from the index
    // (destroyed), so the live snapshot above missed it, but the joiner still has it (from its own
    // save) and must see it clean. value=0 verbatim. (g_wipedKeys is GT-only; the connect edge is GT.)
    int wiped = 0;
    for (const auto& key : g_wipedKeys) {
        coop::net::KeyedScalarPayload p{};
        WireKeyFromString(key, p.key);
        p.value = 0.f;
        p.adopt = 1;  // joiner adopts the host's world -> this decal is clean
        s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
        ++wiped;
    }
    UE_LOGI("grime: connect-snapshot -- sent %d process(es) + %d wiped(=0) to slot %d (of %zu indexed)",
            sent, wiped, peerSlot, total);
}

void Tick() {
    if (!G::EnsureResolved()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        // L5 (FPS): INCREMENTAL rebuild -- tail-scan only the NEW decals (the array TAIL) + a rare ~5min
        // full safety (no 237k walk) + profile it. The cheap g_pending resolution below still runs every
        // throttle (a pending item only resolves once its actor exists in the index).
        ue_wrap::ScopedWalkTimer _wt("grime:RebuildIndex");  // logs only the rare ~5min full safety
        RebuildIndex();
        if (!g_pending.empty()) {
            int applied = 0, expired = 0, still = 0;
            for (auto it = g_pending.begin(); it != g_pending.end();) {
                if (void* actor = ResolveFast(it->first)) {
                    ApplyResolved(actor, it->first, it->second.value, it->second.adopt, it->second.fromSlot);
                    it = g_pending.erase(it);
                    ++applied;
                } else if (now >= it->second.deadline) {
                    if (ProbeLog())
                        UE_LOGI("grime: deferred '%ls' expired (not present on this peer)", it->first.c_str());
                    it = g_pending.erase(it);
                    ++expired;
                } else { ++it; ++still; }
            }
            if (applied || expired)
                UE_LOGI("grime: retry tick -- applied %d deferred, dropped %d expired, %d still pending",
                        applied, expired, still);
        }
    }
    if (now - g_lastPoll >= kPollThrottle) {
        g_lastPoll = now;
        PollAndBroadcast();
    }
}

void OnDisconnect() {
    g_pending.clear();
    g_wipedKeys.clear();
    std::lock_guard<std::mutex> lk(g_stateMutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    if (n > 0) UE_LOGI("grime: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::grime_sync
