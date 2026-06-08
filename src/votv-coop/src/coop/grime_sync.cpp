// coop/grime_sync.cpp -- see coop/grime_sync.h. Surface grime dirt sync.
//
// A sibling of window_sync (the keyed monotone-min-wins scalar channel) keyed by a QUANTIZED
// WORLD-POSITION string instead of an FName Key: Agrime_C is a STATIC level-placed decal, so its
// saved transform is identical across peers (same save) and its position is its cross-peer
// identity. The one engine difference from the window: the apply repaints via Agrime_C::
// applyMaterial (not a pure setter). A wiped-to-completion decal's mirror is driven to process~=0
// (invisible); the FINAL decal removal is NOT propagated (an IsLiveByIndex death-watch was tried
// and REMOVED -- grime streams in/out of sublevels, so a vanished index entry is not a reliable
// destroy signal; the first smoke flooded false destroys on the connect-teleport stream-out). See
// grime_sync.h for the deferred K2_DestroyActor-PRE-edge destroy path.
//
// (window_sync + grime_sync are now two near-identical keyed-float channels; if a third appears,
// generalize into a shared Adapter+Channel the way interactable_sync did for its 3 bool features.)

#include "coop/grime_sync.h"

#include "coop/ini_config.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/engine.h"          // GetActorLocation (the grime's world position -> posKey)
#include "ue_wrap/grime.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
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
    std::unordered_map<void*, std::wstring> nextCache;
    std::vector<std::pair<std::wstring, Ref>> found;
    found.reserve(64);
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!G::IsGrime(obj)) continue;  // cheap class-descendant filter (no alloc)
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;  // skip CDO
        if (!R::IsLive(obj)) continue;
        auto cit = s_posKeyByActor.find(obj);
        std::wstring key = (cit != s_posKeyByActor.end()) ? cit->second : PosKey(obj);
        nextCache.emplace(obj, key);
        found.emplace_back(std::move(key), Ref{ obj, R::InternalIndexOf(obj) });
    }
    s_posKeyByActor.swap(nextCache);  // keep live actors' cached keys, drop dead ones
    uint64_t posHash = 0;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        g_byKey.clear();
        for (auto& f : found) { posHash ^= FnvKey(f.first); g_byKey[f.first] = f.second; }
    }
    if (found.size() != g_lastLogCount || posHash != g_lastLogHash) {
        g_lastLogCount = found.size();
        g_lastLogHash = posHash;
        UE_LOGI("grime: index rebuilt -- %zu live grime decal(s), posHash=0x%016llX "
                "(compare host vs client for cross-peer position stability)",
                found.size(), static_cast<unsigned long long>(posHash));
    }
    if (ProbeLog())
        for (auto& f : found)
            UE_LOGI("grime[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
    return found.size();
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
    {
        std::lock_guard<std::mutex> lkI(g_indexMutex);
        if (g_byKey.empty()) return;
        std::lock_guard<std::mutex> lkS(g_stateMutex);
        for (auto& kv : g_byKey) {
            // A vanished decal (IsLiveByIndex false) is NOT a wipe-destroy: grime decals live in
            // STREAMED sublevels and stream out/in as the player moves (the first smoke proved a
            // connect-teleport streams out hundreds at once). We just skip it -- the throttled rebuild
            // drops it, a stream-back re-primes it. The process MIN-wins stream is inherently
            // streaming-safe (a stream-out does not change process). See grime_sync.h for the deferred
            // process<0 destroy path that a vanished index entry must NOT be used to infer.
            if (!R::IsLiveByIndex(kv.second.actor, kv.second.idx)) continue;
            float cur = 0.f;
            if (!G::ReadProcess(kv.second.actor, cur)) continue;
            auto it = g_lastKnown.find(kv.first);
            if (it == g_lastKnown.end()) { g_lastKnown.emplace(kv.first, cur); continue; }  // prime silently
            if (cur < it->second - kProcessEps) { toSend.emplace_back(kv.first, cur); it->second = cur; }  // a wipe
            else if (cur > it->second + kProcessEps) { it->second = cur; }  // got dirtier (save reload) -- resync silently, never propagate a re-dirty
        }
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
    UE_LOGI("grime: connect-snapshot -- sent %d grime process(es) to slot %d (of %zu indexed)",
            sent, peerSlot, total);
}

void Tick() {
    if (!G::EnsureResolved()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
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
    std::lock_guard<std::mutex> lk(g_stateMutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    if (n > 0) UE_LOGI("grime: OnDisconnect cleared %zu last-known", n);
}

}  // namespace coop::grime_sync
