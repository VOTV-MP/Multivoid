// coop/interactables/grime_sync.cpp -- see coop/interactables/grime_sync.h; the surface grime
// dirt sync. A sibling of the window sync (the keyed monotone-min-wins scalar channel) keyed
// by a quantised world-position string instead of a key name: a grime decal is a static
// level-placed actor, so its saved transform is identical across peers (the same save) and
// its position is its cross-peer identity. The one engine difference from the window: the
// apply repaints through the decal's own apply-material verb, not a pure setter. A wipe is
// seen at the verb that makes it: grime_C::clean, the one writer of `process` a player or the
// weather runs (a sponge stroke, the rain), watched on the script gate before and after its
// body. A clean that takes the process below zero destroys the decal inside its body; the
// engine's end of play reports that destroy with its reason (coop/element/death_seam), so a
// wipe to destruction and a sublevel's stream-out are told apart by what the engine says,
// never by where the camera stands. If a third keyed-float channel appears, generalise the
// window and grime pair into a shared adapter and channel, as the interactable sync did for
// its bool features.

#include "coop/interactables/grime_sync.h"

#include "coop/config/config.h"
#include "coop/element/death_seam.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/engine/engine.h"          // TryGetActorLocation (the grime's world position -> posKey)
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"     // gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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
namespace sg = ue_wrap::script_gate;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

constexpr auto kRetryRebuildThrottle = std::chrono::seconds(2);
constexpr auto kPendingTTL = std::chrono::seconds(25);
// The process changes in discrete wipe steps; this epsilon only filters float-equality noise.
constexpr float kProcessEps = 0.0005f;
// The position quantisation grid (cm). A decal's saved position is bit-identical across peers
// (the same save, a static actor), so any deterministic quantisation yields the same key on
// both; the grid only has to be fine enough that two distinct decals never share a cell, and
// 2 cm plus the type disambiguator is ample.
constexpr double kPosGrid = 2.0;
bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

struct Ref { void* actor; int32_t idx; };
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

std::mutex g_stateMutex;
std::unordered_map<std::wstring, float> g_lastKnown;  // key -> last broadcast/applied process

struct Pending { float value; bool adopt; uint8_t fromSlot; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<std::wstring, Pending> g_pending;  // GT-only: deferred GrimeState applies

// Game thread only: the position keys whose decal we wiped to destruction (the death watch
// fired and we sent zero). The host re-sends zero for these in the connect snapshot, so a
// joiner cleans a decal wiped before it joined (a destroyed decal is gone from the index, so
// the normal snapshot misses it). Bounded by the world's decal count.
std::unordered_set<std::wstring> g_wipedKeys;

std::chrono::steady_clock::time_point g_lastRetry{};  // GT-only: rebuild + deferred-retry throttle
size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only

// The cross-peer identity of a static grime decal: its quantised world position and type.
// The same save gives the identical saved transform, so the identical key on both peers.
// Fits the wire key for any in-base coordinate. Empty when the location read fails: a key is
// never made from the origin a failed read leaves.
std::wstring PosKey(void* grime) {
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(grime, loc)) return std::wstring();
    int32_t type = 0; G::ReadType(grime, type);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    std::wstring k = L"g_";
    k += std::to_wstring(q(loc.X)); k += L'_';
    k += std::to_wstring(q(loc.Y)); k += L'_';
    k += std::to_wstring(q(loc.Z)); k += L'_';
    k += std::to_wstring(type);
    return k;
}

// The world generation of the last completed hub pass; a stale-generation index is treated
// as empty on every read path (the dead-world guard).
uint32_t g_indexGen = 0;
bool IndexCurrent() { return g_indexGen == ue_wrap::world_identity::Generation(); }

void* ResolveFast(const std::wstring& key) {
    if (!IndexCurrent()) return nullptr;  // a stale-generation index is another world's actors
    std::lock_guard<std::mutex> lk(g_indexMutex);
    auto it = g_byKey.find(key);
    if (it != g_byKey.end() && R::IsLiveByIndex(it->second.actor, it->second.idx))
        return it->second.actor;
    return nullptr;
}

// The shared-scan hub consumer: the per-module walk is gone, and the hub's shared sliced pass
// drives these callbacks. Two settle scans rather than fifteen: grime churns whenever the
// player washes (decal deaths re-arm the demand), so fifteen would pin a cleaning session in
// full-demand cadence; new decals append and the tail passes catch them, and recycled-slot
// arrivals heal within the hub's backstop. The key cache: a decal is static, so its key never
// changes, yet computing it dispatches a location UFunction, and doing that for a thousand
// unchanged decals on every rebuild was a recurring frame hitch; only a new decal computes
// it, and a full pass rebuilds the cache from the live set. An entry carries the slot and its
// engine serial (reflection::AllocateSlotSerial, the weak-pointer rule), so a new decal the
// allocator put at a dead one's address in the same slot never inherits the dead one's key.
struct CachedKey {
    std::wstring key;
    int32_t      idx;
    int32_t      serial;
};
std::unordered_map<void*, CachedKey> g_posKeyByActor;   // actor -> cached PosKey (GT-only)
std::unordered_map<void*, CachedKey> g_scanNextCache;   // pass scratch: full-pass cache rebuild
std::vector<std::pair<std::wstring, Ref>> g_scanFound;     // pass scratch (GT-only)
bool g_scanIsFull = false;                                  // pass context

void HubPassBegin(void*, bool isFull) {
    g_scanFound.clear();
    g_scanNextCache.clear();
    g_scanIsFull = isFull;
}

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;  // skip the CDO (alloc-free)
    if (!R::IsLive(obj)) return;
    const int32_t idx = R::InternalIndexOf(obj);
    auto cit = g_posKeyByActor.find(obj);
    CachedKey ck;
    if (cit != g_posKeyByActor.end() && cit->second.idx == idx &&
        R::SlotSerial(idx) == cit->second.serial) {
        ck = cit->second;
    } else {
        ck.key = PosKey(obj);
        if (ck.key.empty()) return;   // no location read, so no identity to index it under
        ck.idx = idx;
        ck.serial = R::AllocateSlotSerial(idx);
    }
    if (g_scanIsFull) g_scanNextCache.insert_or_assign(obj, ck);   // full pass rebuilds the cache
    else              g_posKeyByActor.insert_or_assign(obj, ck);   // tail pass: cache the NEW actor's key
    g_scanFound.emplace_back(std::move(ck.key), Ref{ obj, idx });
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    const size_t added = g_scanFound.size();
    if (isFull) g_posKeyByActor.swap(g_scanNextCache);  // keep live actors' cached keys, drop dead ones
    uint64_t posHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) g_byKey.clear();                           // full pass: rebuild from scratch
        for (auto& f : g_scanFound) g_byKey[f.first] = f.second;
        if (!isFull) {                                         // tail pass: prune dead entries (cheap, O(index))
            for (auto it = g_byKey.begin(); it != g_byKey.end(); ) {
                if (R::IsLiveByIndex(it->second.actor, it->second.idx)) ++it;
                else { g_posKeyByActor.erase(it->second.actor); it = g_byKey.erase(it); }
            }
        }
        for (auto& kv : g_byKey) posHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    if (total != g_lastLogCount || posHash != g_lastLogHash) {
        g_lastLogCount = total;
        g_lastLogHash = posHash;
        UE_LOGI("grime: index rebuilt -- %zu live grime decal(s), posHash=0x%016llX (%s pass, +%zu new) "
                "(compare host vs client for cross-peer position stability)",
                total, static_cast<unsigned long long>(posHash), isFull ? "full" : "tail", added);
    }
    if (ProbeLog())
        for (auto& f : g_scanFound)
            UE_LOGI("grime[probe]: key='%ls' idx=%d actor=%p", f.first.c_str(), f.second.idx, f.second.actor);
    g_scanFound.clear();
    g_scanNextCache.clear();
    return total;
}

void RegisterWithScanHub() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "grime", nullptr, &G::EnsureResolved, &G::IsGrime,
        &HubPassBegin, &HubMatch, &HubPassComplete, /*settleScans*/ 2});
}

// Apply a remote process value: adopt takes the wire value as is (the host connect
// snapshot); otherwise the minimum of local and wire (a live wipe can only clean, never
// re-dirty). Idempotent if already at the target. The apply writes the field and repaints; it
// runs no clean, so it is never heard back as a wipe.
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
    const bool ok = G::WriteProcessAndApply(actor, target);
    { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[key] = target; }
    UE_LOGI("grime: applied process=%.3f (wire=%.3f adopt=%d) ok=%d key='%ls' (from slot %u)",
            target, wireProcess, adopt ? 1 : 0, ok ? 1 : 0, key.c_str(), fromSlot);
}

constexpr int kTagGrimeClean = 0x4752494D;  // 'GRIM'
bool g_watchInstalled = false;
bool g_endSubscribed = false;

// The decal a clean is running on and its process before the body. A clean never nests on one decal.
void* g_cleaning = nullptr;
float g_before = 0.f;

// The decal's cross-peer key, from the index's cache: a decal that is not indexed is runtime splatter,
// which this lane cannot name.
std::wstring KeyOf(void* actor) {
    const auto it = g_posKeyByActor.find(actor);
    return it == g_posKeyByActor.end() ? std::wstring() : it->second.key;
}

void Send(const std::wstring& key, float value) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = value;
    p.adopt = 0;  // a live wipe: receivers apply the minimum
    if (s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p)))
        UE_LOGI("grime: sent process=%.3f key='%ls'", value, key.c_str());
    else
        UE_LOGW("grime: SendReliable failed key='%ls'", key.c_str());
}

sg::Verdict OnCleanPre(const sg::Call& call) {
    g_cleaning = nullptr;
    if (call.object && G::IsGrime(call.object) && G::ReadProcess(call.object, g_before)) g_cleaning = call.object;
    return sg::Verdict::Run;
}

// After the body: the process it left, sent when it fell. A clean that took it below zero destroyed the
// decal inside the body; its end of play sends the wipe instead (OnGrimeEnd).
void OnCleanPost(const sg::Call& call) {
    if (!g_cleaning || call.object != g_cleaning) return;
    g_cleaning = nullptr;
    if (!R::IsLiveByIndex(call.object, R::InternalIndexOf(call.object))) return;
    float cur = 0.f;
    if (!G::ReadProcess(call.object, cur) || cur >= g_before - kProcessEps) return;  // an uncleanable decal
    const std::wstring key = KeyOf(call.object);
    if (key.empty()) return;
    { std::lock_guard<std::mutex> lk(g_stateMutex); g_lastKnown[key] = cur; }
    Send(key, cur);
}

// A decal's end of play. A stream-out only leaves the index (a stream-back re-adds it); a destroy is a
// clean that took the process below zero, sent as a wipe to zero and kept for a joiner's snapshot.
void OnGrimeEnd(const coop::element::death_seam::ActorEnd& end) {
    const std::wstring key = KeyOf(end.actor);
    if (key.empty()) return;
    {
        std::lock_guard<std::mutex> lkI(g_indexMutex);
        std::lock_guard<std::mutex> lkS(g_stateMutex);
        const auto it = g_byKey.find(key);
        if (it != g_byKey.end() && it->second.actor == end.actor) g_byKey.erase(it);
        g_lastKnown.erase(key);
    }
    g_posKeyByActor.erase(end.actor);
    if (end.streamedOut) return;
    g_wipedKeys.insert(key);
    Send(key, 0.f);  // wiped to destruction: the peers' copies go fully clean
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();  // the hub builds the index on its own cadence
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("grime: OnReliable empty key -- dropping"); return; }
    if (!G::EnsureResolved()) {
        UE_LOGW("grime: apply -- class not resolved, dropping key='%ls'", key.c_str());
        return;
    }
    // The trust boundary (the same as the window's): adopt (as is, able to re-dirty) is honoured
    // only from the host; a client edge is forced to a min-wins live wipe.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    if (void* actor = ResolveFast(key)) { ApplyResolved(actor, key, payload.value, adopt, senderPeerSlot); return; }
    // Not streamed in yet: defer and retry (the merge: adopt overrides, live keeps the minimum).
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
    // The hub keeps the index at most one pass (about two seconds) fresh, so no forced rebuild
    // (a decal minted in that window reaches the joiner via its next state change; grime state
    // only ever moves on a wipe, which broadcasts). The index is iterated in place (no per-key
    // string snapshot, as in the poll). The connect edge is game-thread-serial with the rebuild,
    // so holding both locks across the sends is safe.
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
            p.adopt = 1;  // the connect snapshot: the joiner adopts the host's world as is
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            g_lastKnown[kv.first] = process;
            ++sent;
        }
    }
    // Also clean every decal we wiped to destruction this session: it is gone from the index
    // (destroyed), so the live snapshot above missed it, but the joiner still has it from its
    // own save and must see it clean. Zero, adopted.
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
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    // The clean verb's watch and the decals' end of play, each asked again until it takes.
    if (!g_watchInstalled) g_watchInstalled = sg::WatchName(L"clean", kTagGrimeClean, &OnCleanPre, &OnCleanPost);
    if (!g_endSubscribed && coop::element::death_seam::Install())
        g_endSubscribed = coop::element::death_seam::SubscribeClass(L"grime_C", &OnGrimeEnd);
    sg::ResolvePendingNames();
    if (!IndexCurrent()) return;  // index belongs to a dead world -- wait for the hub's next pass
    const auto now = std::chrono::steady_clock::now();
    if (now - g_lastRetry >= kRetryRebuildThrottle) {
        g_lastRetry = now;
        // The cheap pending resolution below still runs every throttle (a pending item only
        // resolves once its actor exists in the hub-maintained index).
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
}

void OnDisconnect() {
    g_pending.clear();
    g_wipedKeys.clear();
    std::lock_guard<std::mutex> lk(g_stateMutex);
    const size_t n = g_lastKnown.size();
    g_lastKnown.clear();
    if (n > 0) UE_LOGI("grime: OnDisconnect cleared %zu last-known", n);
}

std::wstring DebugPosKeyForActor(void* actor) {
    // The dev drill's parity probe: the real quantiser, cache-independent.
    return actor ? PosKey(actor) : std::wstring();
}

}  // namespace coop::grime_sync
