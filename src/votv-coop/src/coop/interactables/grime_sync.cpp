// coop/interactables/grime_sync.cpp -- see coop/interactables/grime_sync.h; the surface grime
// dirt sync. A sibling of the window sync (the keyed monotone-min-wins scalar channel) keyed
// by a quantised world-position string instead of a key name: the save gives every decal it
// holds an identical transform on each peer, so its position is its cross-peer identity. The
// one engine difference from the window: the apply repaints through the decal's own
// apply-material verb, not a pure setter. A fall is seen at the verbs that make one, watched on
// the script gate before and after their bodies: grime_C::clean (a sponge stroke, the rain) and
// the wall fixer's repair of a crack (wallFixer_fix), the two verbs that lower `process`. A verb
// that takes it below zero destroys the decal inside its body; the engine's end of play reports
// that destroy with its reason (coop/element/death_seam), so a wipe to destruction and a
// sublevel's stream-out are told apart by what the engine says, never by where the camera
// stands. If a third keyed-float channel appears, generalise the window and grime pair into a
// shared adapter and channel, as the interactable sync did for its bool features.

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

#include <atomic>
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
namespace sg = ue_wrap::script_gate;
namespace DS = coop::element::death_seam;

using coop::net::WireKeyFromString;
using coop::net::StringFromWireKey;
using coop::net::FnvKey;

// The process changes in wipe steps, and a step can be smaller than this epsilon, so a fall is
// measured against the floor and small ones add up (OnLowerPost); it also skips an idempotent apply.
constexpr float kProcessEps = 0.0005f;
// The position quantisation grid (cm). A decal's saved position is bit-identical across peers
// (the same save, a static actor), so any deterministic quantisation yields the same key on
// both; the grid has to be fine enough that distinct decals rarely share a cell. The rig's save puts
// 1117 decals on 1087 keys: 30 stand in a cell another of their type holds, and the lane treats a
// cell's decals as one -- the floor is kept per key, so a fall on one lowers its twins at the next full
// pass.
constexpr double kPosGrid = 2.0;
bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

// An indexed decal: the actor, its slot and the serial the slot held when a pass took it (the key cache's
// own), so a new object the allocator put at the address in the same slot is never read as the decal.
struct Ref { void* actor; int32_t idx; int32_t serial; };
bool Holds(const Ref& r) { return R::IsLiveByIndex(r.actor, r.idx) && R::SlotSerial(r.idx) == r.serial; }
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

// A per-key register of the running world: another world's is dropped the first time it is touched in
// this one, a value that arrives before the new world's first pass included. A peer leaving does not
// clear it. Game thread only.
struct WorldRegister {
    std::unordered_map<std::wstring, float> map;
    uint32_t gen = 0;
    std::unordered_map<std::wstring, float>& Get() {
        const uint32_t now = ue_wrap::world_identity::Generation();
        if (now != gen) {
            map.clear();
            gen = now;
        }
        return map;
    }
};

// The floor: per key, the lowest process this peer knows the decal at in this world -- a fall it made
// or was sent, a destroy (zero), the value it streamed out at, or the host's snapshot as adopted. It is
// the lane's register: a decal the index takes (a stream-back, one the join brought, one that was not
// here when its value arrived) is lowered to it, and the host's snapshot sends it for every key its
// index does not hold.
WorldRegister g_floor;
std::unordered_map<std::wstring, float>& Floor() { return g_floor.Get(); }

// Per key with no floor yet, the process its decal had when this peer's first fall on it began: what
// every peer holds for a key nobody has moved in this world, so falls under the epsilon add up from it.
// Kept apart from the floor, which lowers every decal of the key's cell and carries only values that
// were sent, so a fall that has not gone out moves nothing but its own decal.
WorldRegister g_unmoved;

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
    if (it != g_byKey.end() && Holds(it->second))
        return it->second.actor;
    return nullptr;
}

// Lowers the floor of `key` to `value`; true when it moved.
bool Lower(const std::wstring& key, float value) {
    auto& floor = Floor();
    const auto it = floor.find(key);
    if (it != floor.end() && value >= it->second - kProcessEps) return false;
    floor[key] = value;
    return true;
}

// Write `target` into a live decal and repaint it, unless it already reads that -- or, `lowerOnly`, unless
// it already reads that or less: a fall lowers a decal and never raises one this peer cleaned further.
// The apply runs no verb, so it is never heard back as a fall.
void ApplyResolved(void* actor, const std::wstring& key, float target, const char* why, bool lowerOnly) {
    float cur = 0.f;
    if (!G::ReadProcess(actor, cur) || std::fabs(target - cur) < kProcessEps) return;
    if (lowerOnly && cur < target) return;
    const bool ok = G::WriteProcessAndApply(actor, target);
    if (ProbeLog() || !ok)
        UE_LOGI("grime: applied process=%.3f (was %.3f, %s) ok=%d key='%ls'", target, cur, why, ok ? 1 : 0,
                key.c_str());
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
    const int32_t serial = ck.serial;
    g_scanFound.emplace_back(std::move(ck.key), Ref{ obj, idx, serial });
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
                if (Holds(it->second)) ++it;
                else { g_posKeyByActor.erase(it->second.actor); it = g_byKey.erase(it); }
            }
        }
        for (auto& kv : g_byKey) posHash ^= FnvKey(kv.first);  // recompute over the index (cheap, O(index))
        total = g_byKey.size();
        g_indexGen = worldGen;
    }
    // Every decal this pass found is lowered to its floor: one that streamed back in, one a join brought
    // after its value arrived, one this peer had not indexed when a peer's fall came. A pass spans frames,
    // so a decal matched early may have ended since: only one still live is read.
    size_t lowered = 0;
    auto& floor = Floor();
    if (!floor.empty()) {
        for (const auto& f : g_scanFound) {
            if (!Holds(f.second)) continue;
            const auto it = floor.find(f.first);
            if (it == floor.end()) continue;
            float cur = 0.f;
            if (!G::ReadProcess(f.second.actor, cur) || cur <= it->second + kProcessEps) continue;
            ApplyResolved(f.second.actor, f.first, it->second, "floor at index", /*lowerOnly=*/true);
            ++lowered;
        }
    }
    if (total != g_lastLogCount || posHash != g_lastLogHash || lowered > 0) {
        g_lastLogCount = total;
        g_lastLogHash = posHash;
        UE_LOGI("grime: index rebuilt -- %zu live grime decal(s), posHash=0x%016llX (%s pass, +%zu new, %zu lowered "
                "to their floor) (compare host vs client for cross-peer position stability)",
                total, static_cast<unsigned long long>(posHash), isFull ? "full" : "tail", added, lowered);
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

constexpr int kTagGrimeClean = 0x4752494D;  // 'GRIM'
constexpr int kTagGrimeFix   = 0x47524958;  // 'GRIX'
bool g_cleanWatched = false;
bool g_fixWatched = false;
bool g_endSubscribed = false;

// The decal a verb is running on and its process before the body. A verb never nests on one decal.
void* g_lowering = nullptr;
float g_before = 0.f;

// The decal's cross-peer key, from the index's cache: only for a decal the current world's index holds
// under the slot and serial it still carries. A decal no pass has reached yet has none, and its fall is
// not sent; the floor of the peer it reaches next is the other side of that.
std::wstring KeyOf(void* actor, int32_t idx, int32_t serial) {
    if (!IndexCurrent()) return std::wstring();
    const auto it = g_posKeyByActor.find(actor);
    if (it == g_posKeyByActor.end()) return std::wstring();
    const CachedKey& ck = it->second;
    if (ck.idx != idx || ck.serial == 0 || ck.serial != serial) return std::wstring();
    return ck.key;
}

void Send(const std::wstring& key, float value) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = value;
    p.adopt = 0;  // a live fall: receivers apply the minimum
    if (!s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p)))
        UE_LOGW("grime: SendReliable failed key='%ls'", key.c_str());
    else if (ProbeLog())
        UE_LOGI("grime: sent process=%.3f key='%ls'", value, key.c_str());
}

sg::Verdict OnLowerPre(const sg::Call& call) {
    g_lowering = nullptr;
    if (call.object && G::IsGrime(call.object) && G::ReadProcess(call.object, g_before)) g_lowering = call.object;
    return sg::Verdict::Run;
}

// After the body: the process it left, sent when it fell. A verb that took it below zero destroyed the
// decal inside the body; its end of play sends the wipe instead (OnGrimeEnd).
void OnLowerPost(const sg::Call& call) {
    if (!g_lowering || call.object != g_lowering) return;
    g_lowering = nullptr;
    const int32_t idx = R::InternalIndexOf(call.object);
    if (!R::IsLiveByIndex(call.object, idx)) return;
    float cur = 0.f;
    if (!G::ReadProcess(call.object, cur) || cur >= g_before) return;  // an uncleanable decal
    const std::wstring key = KeyOf(call.object, idx, R::SlotSerial(idx));
    if (key.empty()) return;
    // Measured against the floor, the lowest value this peer knows for the key, or, before the key has
    // one, the process its decal had when this peer's first fall on it began: falls smaller than the
    // epsilon add up and go out once they pass it.
    auto& floor = Floor();
    const auto it = floor.find(key);
    auto& unmoved = g_unmoved.Get();
    const float from = it != floor.end() ? it->second : unmoved.emplace(key, g_before).first->second;
    if (cur >= from - kProcessEps) return;
    Lower(key, cur);
    unmoved.erase(key);
    Send(key, cur);
}

// A decal's end of play. A stream-out leaves the index and keeps its process as the floor, for its
// stream-back; a destroy -- a clean or a repair past zero, leaves raked or expired, fuel burnt out -- ends
// the decal on this peer, sent as a wipe to zero and kept as the floor for a joiner's snapshot.
void OnGrimeEnd(const DS::ActorEnd& end) {
    const std::wstring key = KeyOf(end.actor, end.actorIndex, end.actorSerial);
    if (key.empty()) return;
    // The actor ended before this drain; its memory is read only while its slot still holds it under the
    // serial it ended with, which a purge in between resets.
    float at = 0.f;
    const bool held = R::ObjectAt(end.actorIndex) == end.actor && R::SlotSerial(end.actorIndex) == end.actorSerial;
    const bool read = end.streamedOut && held && G::ReadProcess(end.actor, at);
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        const auto it = g_byKey.find(key);
        if (it != g_byKey.end() && it->second.actor == end.actor) g_byKey.erase(it);
    }
    g_posKeyByActor.erase(end.actor);
    if (end.streamedOut) {
        if (read) Lower(key, at);
        return;
    }
    Lower(key, 0.f);
    Send(key, 0.f);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    RegisterWithScanHub();  // the hub builds the index on its own cadence
}

void OnReliable(const coop::net::KeyedScalarPayload& payload, uint8_t senderPeerSlot) {
    std::wstring key = StringFromWireKey(payload.key);
    if (key.empty()) { UE_LOGW("grime: OnReliable empty key -- dropping"); return; }
    // The trust boundary (the same as the window's): adopt (as is, able to re-dirty) is honoured
    // only from the host; a client edge is forced to a min-wins live fall.
    const bool adopt = (payload.adopt != 0) && (senderPeerSlot == 0);
    // The floor takes the value whether or not the decal is here to show it. A peer's fall is written only
    // to a decal reading above it, and a pass lowers a decal to its floor and never raises one, so the host's
    // snapshot raises only a decal this peer indexes when the value arrives.
    auto& floor = Floor();
    if (adopt) floor[key] = payload.value;
    else if (!Lower(key, payload.value)) return;  // not below what this peer already holds
    if (void* actor = ResolveFast(key))
        ApplyResolved(actor, key, floor[key], adopt ? "adopted" : "a peer's fall", /*lowerOnly=*/!adopt);
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    if (s->role() != coop::net::Role::Host) return;  // host-only snapshot
    if (peerSlot < 0 || peerSlot >= static_cast<int>(coop::players::kMaxPeers)) return;
    if (!IndexCurrent()) {
        UE_LOGW("grime: connect-snapshot for slot %d skipped -- the index belongs to another world", peerSlot);
        return;
    }
    // The hub keeps the index at most one pass (about two seconds) fresh, so no forced rebuild: a
    // decal minted in that window reaches the joiner by its next fall. The connect edge is
    // game-thread-serial with the rebuild, so holding the lock across the sends is safe.
    int sent = 0;
    int floors = 0;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        total = g_byKey.size();
        auto& floor = Floor();
        for (auto& kv : g_byKey) {
            if (!Holds(kv.second)) continue;
            float process = 0.f;
            if (!G::ReadProcess(kv.second.actor, process)) continue;
            // The lower of the decal and its floor: a cell's twin the host cleaned lowers the key until the
            // next full pass lowers this one too.
            const auto fl = floor.find(kv.first);
            if (fl != floor.end() && fl->second < process) process = fl->second;
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(kv.first, p.key);
            p.value = process;
            p.adopt = 1;  // the connect snapshot: the joiner adopts the host's world as is
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            ++sent;
        }
        // And the floor of every key the index does not hold live: a decal ended here -- zero -- or
        // streamed out, which the joiner has from its own copy of the save and must see as this world
        // has it.
        for (const auto& kv : floor) {
            const auto idx = g_byKey.find(kv.first);
            if (idx != g_byKey.end() && Holds(idx->second)) continue;
            coop::net::KeyedScalarPayload p{};
            WireKeyFromString(kv.first, p.key);
            p.value = kv.second;
            p.adopt = 1;
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
            ++floors;
        }
    }
    UE_LOGI("grime: connect-snapshot -- sent %d live process(es) + %d floor(s) of decals not indexed to slot %d "
            "(of %zu indexed)", sent, floors, peerSlot, total);
}

void Tick() {
    if (!G::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    // The two lowering verbs' watches and the decals' end of play, each asked again until it takes. A
    // name watch matches the verb on any class; the pre half keeps only a grime decal.
    if (!g_cleanWatched) g_cleanWatched = sg::WatchName(L"clean", kTagGrimeClean, &OnLowerPre, &OnLowerPost);
    if (!g_fixWatched) g_fixWatched = sg::WatchName(L"wallFixer_fix", kTagGrimeFix, &OnLowerPre, &OnLowerPost);
    if (!g_endSubscribed && DS::Install()) g_endSubscribed = DS::SubscribeClass(L"grime_C", &OnGrimeEnd);
    sg::ResolvePendingNames();
}

std::wstring DebugPosKeyForActor(void* actor) {
    // The dev drill's parity probe: the real quantiser, cache-independent.
    return actor ? PosKey(actor) : std::wstring();
}

}  // namespace coop::grime_sync
