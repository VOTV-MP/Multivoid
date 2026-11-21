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
#include "coop/interactables/grime_judge.h"
#include "coop/net/protocol.h"
#include "coop/net/refusal_tally.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"  // WireKeyFromString / StringFromWireKey / FnvKey (shared)
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers

#include "ue_wrap/engine/engine.h"          // TryGetActorLocation (the grime's world position -> posKey)
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/engine/world_identity.h"     // gen-stamped index (dead-world guard)
#include "coop/element/object_scan_hub.h"      // the shared sliced scan pass

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
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
// 1116 decals on 1086 keys: 30 stand in a place another decal holds, nearly all of the same class, and
// the lane treats a place's decals as one -- the floor is kept per key, so a fall on one lowers its twins
// at the next full pass. That is by design: the game's clean acts on a place, so the place is the identity
// (a class in the key would only have lengthened it toward the wire's limit). The census line of a world's
// first full pass counts the places that are shared.
constexpr double kPosGrid = 2.0;
// The wire key holds this many characters and WireKeyFromString cuts a longer one; a longer key is refused, not cut.
constexpr size_t kWireKeyChars = sizeof(coop::net::WireKey::data);
size_t g_longestKey = 0;       // the longest key KeysOf ever made (GT-only)
uint32_t g_keysRefused = 0;    // keys refused for their length (GT-only)
uint32_t g_indexNowTwins = 0;  // spawn-time decals not indexed because a live twin holds their key (GT-only)
bool ProbeLog() {
    static const bool s_enabled = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_log);
    return s_enabled;
}

std::atomic<coop::net::Session*> g_session{nullptr};

// What the lane has done, for Tallies. Written on the game thread, read on any.
std::atomic<uint32_t> g_replays{0}, g_passes{0}, g_lowered{0}, g_endsSent{0}, g_passGen{0};
// A save replay's window: open from its begin until kReplayWindowPasses index passes have completed after it. The decals the
// lane lowers inside it are the ones the replay's deletes can have floored (Tallies counts only those).
std::atomic<bool> g_replayWindow{false};
constexpr uint32_t kReplayWindowPasses = 3;

// An indexed decal: the actor, its slot and the serial the slot held when a pass took it (the key cache's
// own), so a new object the allocator put at the address in the same slot is never read as the decal.
struct Ref { void* actor; int32_t idx; int32_t serial; };
bool Holds(const Ref& r) { return R::IsLiveByIndex(r.actor, r.idx) && R::SlotSerial(r.idx) == r.serial; }
std::mutex g_indexMutex;
std::unordered_map<std::wstring, Ref> g_byKey;

// This peer's own name for one decal: its location's float bits and its type. It tells a cell's twins
// apart and never travels, so a value is enough, with no string.
struct ExactKey {
    uint32_t x = 0, y = 0, z = 0;
    int32_t type = 0;
    bool operator==(const ExactKey& o) const { return x == o.x && y == o.y && z == o.z && type == o.type; }
};
struct ExactKeyHash {
    size_t operator()(const ExactKey& k) const {
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the four words
        for (uint32_t w : {k.x, k.y, k.z, static_cast<uint32_t>(k.type)}) h = (h ^ w) * 1099511628211ull;
        // A word-wise multiply leaves the low bits a function of the words' low bits alone, and a round
        // coordinate's float has them all zero; the table picks its bucket from the low bits. murmur3's
        // 64-bit finalizer spreads every bit into them.
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return static_cast<size_t>(h);
    }
};

// A register of the running world: another world's is dropped the first time it is touched in this
// one, a value that arrives before the new world's first pass included. A peer leaving does not clear
// it. Game thread only.
template <class K, class V, class H = std::hash<K>>
struct WorldRegister {
    std::unordered_map<K, V, H> map;
    uint32_t gen = 0;
    std::unordered_map<K, V, H>& Get() {
        const uint32_t now = ue_wrap::world_identity::Generation();
        if (now != gen) {
            map.clear();
            gen = now;
        }
        return map;
    }
};

// The floor: per key, the lowest process every peer knows the key's decals at in this world -- a fall
// that was sent, by this peer or another, a destroy (kGrimeGone, sent as a wipe), or the host's snapshot as
// adopted. It is the lane's shared register: a decal the index takes (a stream-back, one the join
// brought, one that was not here when its value arrived) is lowered to it, every decal of the key's cell
// alike, and the host's snapshot sends it for every key its index does not hold. A value only this peer
// holds enters it only while no peer is connected, a solo host's falls, which the join snapshot sends.
// A key is a place, and twins at one place share it by design: the game's clean acts on a place, so what is
// cleaned or ended there is cleaned or ended for the decals standing in it, and every peer agrees on that.
WorldRegister<std::wstring, float> g_floor;
std::unordered_map<std::wstring, float>& Floor() { return g_floor.Get(); }

// Per decal (ExactKey), for a key with no floor yet: the process the decal had when this peer's first
// fall on it began, what every peer holds for a decal nobody has moved in this world, so falls under the
// epsilon add up from it.
WorldRegister<ExactKey, float, ExactKeyHash> g_unmoved;

// Per decal (its exact place), the process it had when it streamed out, with its key: the stream-back
// comes in at the level's value, and the pass that indexes it restores exactly that decal from here. The
// host's snapshot also sends it for a key its index does not hold. Kept out of the floor, which would
// lower the decal's cell-mates to a value only this peer holds.
struct Parked {
    std::wstring key;
    float value;
};
WorldRegister<ExactKey, Parked, ExactKeyHash> g_parked;

size_t g_lastLogCount = SIZE_MAX;  // GT-only: dedup the rebuilt log
uint64_t g_lastLogHash = 0;        // GT-only

// The cross-peer identity of a static grime decal: its quantised world position and type.
// The same save gives the identical saved transform, so the identical key on both peers.
// Fits the wire key for any in-base coordinate. `exact` is this peer's own name for the one decal
// (ExactKey). False when the location read fails: a key is never made from the origin a failed read
// leaves.
bool KeysOf(void* grime, std::wstring& key, ExactKey& exact) {
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(grime, loc)) return false;
    int32_t type = 0; G::ReadType(grime, type);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    key = L"g_";
    key += std::to_wstring(q(loc.X)); key += L'_';
    key += std::to_wstring(q(loc.Y)); key += L'_';
    key += std::to_wstring(q(loc.Z)); key += L'_';
    key += std::to_wstring(type);
    g_longestKey = std::max(g_longestKey, key.size());
    if (key.size() > kWireKeyChars) {   // the wire would cut it, and no peer's floor would match: the decal stays local
        static bool s_warned = false;
        ++g_keysRefused;
        if (!s_warned) {
            s_warned = true;
            UE_LOGW("grime: a decal's key is %zu characters, past the wire's %zu: it is not synced (the longest so far %zu)",
                    key.size(), kWireKeyChars, g_longestKey);
        }
        return false;
    }
    std::memcpy(&exact.x, &loc.X, sizeof(exact.x));
    std::memcpy(&exact.y, &loc.Y, sizeof(exact.y));
    std::memcpy(&exact.z, &loc.Z, sizeof(exact.z));
    exact.type = type;
    return true;
}

std::wstring PosKey(void* grime) {
    std::wstring key;
    ExactKey exact;
    return KeysOf(grime, key, exact) ? key : std::wstring();
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
    // A decal that ended on another peer ends here too, as the game's clean ends it below zero. It leaves the index
    // first, so its own end of play here is no wipe of this peer's and is not sent again.
    if (target <= coop::net::kGrimeGone + kProcessEps) {
        coop::grime_sync::ForgetDecal(actor);
        const coop::grime_judge::AllowDestroy allow;   // this peer's own apply of the host's end: the hold lets it through
        const bool ok = E::DestroyActor(actor);
        if (g_replayWindow.load(std::memory_order_relaxed)) g_lowered.fetch_add(1, std::memory_order_relaxed);
        UE_LOGI("grime: a decal another peer ended is destroyed here (%s) ok=%d key='%ls'", why, ok ? 1 : 0, key.c_str());
        return;
    }
    float cur = 0.f;
    if (!G::ReadProcess(actor, cur) || std::fabs(target - cur) < kProcessEps) return;
    if (lowerOnly && cur < target) return;
    const bool ok = G::WriteProcessAndApply(actor, target);
    if (g_replayWindow.load(std::memory_order_relaxed)) g_lowered.fetch_add(1, std::memory_order_relaxed);
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
    ExactKey     exact;  // KeysOf's own name for the one decal
    int32_t      idx;
    int32_t      serial;
    bool         provisional = false;   // made at the decal's spawn (IndexNow): the next pass makes it again
};
std::unordered_map<void*, CachedKey> g_posKeyByActor;   // actor -> cached keys (GT-only)
std::unordered_map<void*, CachedKey> g_scanNextCache;   // pass scratch: full-pass cache rebuild
struct Found {
    std::wstring key;
    ExactKey     exact;
    Ref          ref;
};
std::vector<Found> g_scanFound;  // pass scratch (GT-only)
bool g_scanIsFull = false;                                  // pass context
// Bumped as a save replay begins (OnLoadObjects): a pass begun before it found the decals the replay deletes.
uint32_t g_replayEpoch = 0;
uint32_t g_passEpoch = 0;

// What the full pass found, by key: once per world, the places that more than one decal holds (twins share their
// place's key by design, so this is a census and no fault), and the longest key against the wire's limit.
void KeyCensus(uint32_t worldGen) {
    static uint32_t s_gen = 0;
    if (s_gen == worldGen || g_scanFound.empty()) return;
    s_gen = worldGen;
    std::unordered_map<std::wstring, int> byKey;
    size_t longest = 0;
    for (const auto& f : g_scanFound) {
        ++byKey[f.key];
        longest = std::max(longest, f.key.size());
    }
    size_t shared = 0, extra = 0;
    for (const auto& kv : byKey) if (kv.second > 1) { ++shared; extra += static_cast<size_t>(kv.second - 1); }
    UE_LOGI("grime: key census -- %zu decal(s) on %zu key(s): %zu place(s) hold more than one decal (%zu extra, twins "
            "that share their place's key); the longest key is %zu of the wire's %zu characters (the longest ever made: "
            "%zu; %u refused as too long, %u spawn-time twin(s) kept out)", g_scanFound.size(), byKey.size(), shared,
            extra, longest, kWireKeyChars, g_longestKey, g_keysRefused, g_indexNowTwins);
}

void HubPassBegin(void*, bool isFull) {
    g_scanFound.clear();
    g_scanNextCache.clear();
    g_scanIsFull = isFull;
    g_passEpoch = g_replayEpoch;
}

void HubMatch(void*, void* obj) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;  // skip the CDO (alloc-free)
    if (!R::IsLive(obj)) return;
    const int32_t idx = R::InternalIndexOf(obj);
    auto cit = g_posKeyByActor.find(obj);
    CachedKey ck;
    if (cit != g_posKeyByActor.end() && !cit->second.provisional && cit->second.idx == idx &&
        R::SlotSerial(idx) == cit->second.serial) {
        ck = cit->second;
    } else {
        if (!KeysOf(obj, ck.key, ck.exact)) return;  // no location read, so no identity to index it under
        ck.idx = idx;
        ck.serial = R::AllocateSlotSerial(idx);
    }
    if (g_scanIsFull) g_scanNextCache.insert_or_assign(obj, ck);   // full pass rebuilds the cache
    else              g_posKeyByActor.insert_or_assign(obj, ck);   // tail pass: cache the NEW actor's key
    const int32_t serial = ck.serial;
    g_scanFound.push_back(Found{std::move(ck.key), ck.exact, Ref{obj, idx, serial}});
}

size_t HubPassComplete(void*, bool isFull, uint32_t worldGen) {
    if (g_passEpoch != g_replayEpoch) {  // a save replay began inside this pass: what it found is being deleted
        g_scanFound.clear();
        g_scanNextCache.clear();
        std::lock_guard<std::mutex> lk(g_indexMutex);
        return g_byKey.size();
    }
    if (g_passGen.load(std::memory_order_relaxed) != worldGen) {   // a new world counts its passes from none
        g_passGen.store(worldGen, std::memory_order_relaxed);
        g_passes.store(0, std::memory_order_relaxed);
    }
    g_passes.fetch_add(1, std::memory_order_relaxed);
    if (isFull) KeyCensus(worldGen);
    const size_t added = g_scanFound.size();
    if (isFull) g_posKeyByActor.swap(g_scanNextCache);  // keep live actors' cached keys, drop dead ones
    uint64_t posHash = 0;
    size_t   total;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        if (isFull) g_byKey.clear();                           // full pass: rebuild from scratch
        for (auto& f : g_scanFound) g_byKey[f.key] = f.ref;
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
            if (!Holds(f.ref)) continue;
            const auto it = floor.find(f.key);
            if (it == floor.end()) continue;
            float cur = 0.f;
            if (!G::ReadProcess(f.ref.actor, cur) || cur <= it->second + kProcessEps) continue;
            ApplyResolved(f.ref.actor, f.key, it->second, "floor at index", /*lowerOnly=*/true);
            ++lowered;
        }
    }
    // A decal that streamed back in takes the process it streamed out at, this decal alone.
    auto& parked = g_parked.Get();
    if (!parked.empty()) {
        for (const auto& f : g_scanFound) {
            const auto it = parked.find(f.exact);
            if (it == parked.end() || !Holds(f.ref)) continue;
            ApplyResolved(f.ref.actor, f.key, it->second.value, "its stream-out", /*lowerOnly=*/true);
            parked.erase(it);
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
            UE_LOGI("grime[probe]: key='%ls' idx=%d actor=%p", f.key.c_str(), f.ref.idx, f.ref.actor);
    // The replay's window closes with its kReplayWindowPasses-th pass, after that pass's own lowerings were counted.
    if (g_passes.load(std::memory_order_relaxed) >= kReplayWindowPasses)
        g_replayWindow.store(false, std::memory_order_relaxed);
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

UE_FACT_FN(kRowClean, "grime_sync", nullptr, L"clean", AnyClass);
UE_FACT_FN(kRowWallFixerFix, "grime_sync", nullptr, L"wallFixer_fix", AnyClass);
constexpr int kTagGrimeClean = 0x4752494D;  // 'GRIM'
constexpr int kTagGrimeFix   = 0x47524958;  // 'GRIX'
bool g_cleanWatched = false;
bool g_fixWatched = false;
bool g_endSubscribed = false;

// The save replay: the gamemode's loadObjects deletes the level's own decals and spawns the save's in their
// places, at the same keys. Those deletes are no wipe, but an index that had reached the level's decals took
// each for a wipe to zero, and the save's decals at its keys were lowered to it: a rehost in the same game
// process came up with most of its grime gone (Ghezra/Multivoid#36), the first host of a process only when its
// first pass beat the replay. As the replay begins, the index and this world's registers are forgotten.
UE_FACT_FN(kRowLoadObjects, "grime_sync", L"mainGamemode_C", L"loadObjects", Declared);
constexpr int kTagGrimeReplay = 0x47524C4F;  // 'GRLO'
bool g_replayWatched = false;

// The rehost churn's red arm: the replay is counted but the lane forgets nothing, as before the watch.
bool ReplayWatchOff() {
    static const bool off = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_replay_watch_off);
    return off;
}

sg::Verdict OnLoadObjectsPre(const sg::Call&) {
    g_replays.fetch_add(1, std::memory_order_relaxed);
    g_replayWindow.store(true, std::memory_order_relaxed);
    g_passes.store(0, std::memory_order_relaxed);
    if (ReplayWatchOff()) return sg::Verdict::Run;
    ++g_replayEpoch;
    size_t indexed = 0;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        indexed = g_byKey.size();
        g_byKey.clear();
    }
    g_posKeyByActor.clear();
    g_scanFound.clear();
    g_scanNextCache.clear();
    const size_t floors = Floor().size();
    Floor().clear();
    g_unmoved.Get().clear();
    g_parked.Get().clear();
    UE_LOGI("grime: the save replay begins -- the index forgets %zu decal(s) and %zu floor(s); its deletes are no wipe",
            indexed, floors);
    return sg::Verdict::Run;
}

// The decal a verb is running on and its process before the body. A verb never nests on one decal.
void* g_lowering = nullptr;
float g_before = 0.f;

// The decal's cross-peer key, from the index's cache: only for a decal the current world's index holds
// under the slot and serial it still carries. A decal no pass has reached yet has none, and its fall is
// not sent; the floor of the peer it reaches next is the other side of that.
const CachedKey* CachedOf(void* actor, int32_t idx, int32_t serial) {
    if (!IndexCurrent()) return nullptr;
    const auto it = g_posKeyByActor.find(actor);
    if (it == g_posKeyByActor.end()) return nullptr;
    const CachedKey& ck = it->second;
    if (ck.idx != idx || ck.serial == 0 || ck.serial != serial) return nullptr;
    return &ck;
}

std::wstring KeyOf(void* actor, int32_t idx, int32_t serial) {
    const CachedKey* ck = CachedOf(actor, idx, serial);
    return ck ? ck->key : std::wstring();
}

// A live fall, to the host (a client's ask) or to every client (the host's own). `skipSlot` leaves one client out: the
// asker of a granted fall, whose own copy already stands at what it asked.
void Send(const std::wstring& key, float value, int skipSlot = -1) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = value;
    p.adopt = 0;  // a live fall: receivers apply the minimum
    if (!s->SendReliable(coop::net::ReliableKind::GrimeState, &p, sizeof(p), skipSlot))
        UE_LOGW("grime: SendReliable failed key='%ls'", key.c_str());
    else if (ProbeLog())
        UE_LOGI("grime: sent process=%.3f key='%ls'", value, key.c_str());
}

sg::Verdict OnLowerPre(const sg::Call& call) {
    g_lowering = nullptr;
    if (call.object && G::IsGrime(call.object) && G::ReadProcess(call.object, g_before)) {
        g_lowering = call.object;
        // The body carries the judge's scope bit: on a client a destroy the seam holds inside it is told from any other by
        // the frame that makes it, and asks the host (coop/interactables/grime_judge).
        if (const uintptr_t bit = coop::grime_judge::OpenVerb()) sg::MarkScope(call, bit);
    }
    return sg::Verdict::Run;
}

// After the body: the process it left, sent when it fell. A verb that took it below zero destroys the decal inside the
// body. On the host that end of play sends the wipe (OnGrimeEnd). On a client the destroy was held: the process stands
// below zero, which the game's clean leaves for the destroy to settle, so the held decal is written to zero here, the
// stroke's own report, and zero is asked of the host, which destroys at a granted zero; its end of play ends the decal
// on every peer, this one included. With the hold off (its seam did not go on) the decal is already dead here: zero is
// asked after the fact, so the host still ends it.
void OnLowerPost(const sg::Call& call) {
    if (!g_lowering || call.object != g_lowering) return;
    g_lowering = nullptr;
    const bool held = coop::grime_judge::TakeHeld(call.object);
    const int32_t idx = R::InternalIndexOf(call.object);
    if (!R::IsLiveByIndex(call.object, idx)) {
        auto* s = g_session.load(std::memory_order_acquire);
        if (!coop::grime_judge::HoldIsOff() || !s || !s->connected() || s->role() != coop::net::Role::Client) return;
        const CachedKey* dead = CachedOf(call.object, idx, R::SlotSerial(idx));
        if (!dead) return;
        const std::wstring deadKey = dead->key;
        Lower(deadKey, 0.f);
        Send(deadKey, 0.f);
        return;
    }
    float cur = 0.f;
    const CachedKey* ck = CachedOf(call.object, idx, R::SlotSerial(idx));
    if (!ck && held) G::WriteProcessAndApply(call.object, 0.f);   // a decal the lane has no name for stays at zero, asking no one
    if (!ck) return;
    if (held) {
        if (!G::WriteProcessAndApply(call.object, 0.f)) return;
        cur = 0.f;
    } else if (!G::ReadProcess(call.object, cur) || cur >= g_before) {
        return;  // an uncleanable decal
    }
    const std::wstring key = ck->key;
    // Measured against the floor, the lowest value every peer knows for the key, or, before the key has
    // one, the process this decal had when this peer's first fall on it began: falls smaller than the
    // epsilon add up and go out once they pass it.
    auto& floor = Floor();
    const auto it = floor.find(key);
    auto& unmoved = g_unmoved.Get();
    const float from = it != floor.end() ? it->second : unmoved.emplace(ck->exact, g_before).first->second;
    if (cur >= from - kProcessEps) return;
    unmoved.erase(ck->exact);
    Lower(key, cur);
    Send(key, cur);
}

// The clean drill's red arm: a decal's end goes out as a fall to zero, the way it did before kGrimeGone, which every other
// peer's copy takes for a decal at no dirt and leaves standing.
bool GoneOff() {
    static const bool off = ::coop::config::ResolveFlag(::coop::config_registry::rows::grime_gone_off);
    return off;
}

// A decal's end of play. A stream-out leaves the index and parks its process under its exact place,
// for its stream-back; a destroy -- a clean or a repair past zero, leaves raked or expired, fuel burnt
// out -- ends the decal on this peer, sent as a wipe (kGrimeGone, which ends it on every peer) and kept as the floor for a joiner's
// snapshot. A client sends no wipe: a decal ends for everyone only by the host's word, which the host's judge refuses to
// take from a client, so a client's decal that ends here (a world teardown, a destroy no hold covers) is its own.
void OnGrimeEnd(const DS::ActorEnd& end) {
    const CachedKey* ck = CachedOf(end.actor, end.actorIndex, end.actorSerial);
    if (!ck) return;
    const std::wstring key = ck->key;
    const ExactKey exact = ck->exact;
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
        if (read) g_parked.Get().insert_or_assign(exact, Parked{key, at});
        return;
    }
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Client) return;
    const float ended = GoneOff() ? 0.f : coop::net::kGrimeGone;
    Lower(key, ended);
    Send(key, ended);
    g_endsSent.fetch_add(1, std::memory_order_relaxed);
}

// The values refused at the receive edge: the first after a quiet spell is said at once, the rest of a burst summed.
coop::net::RefusalTally g_refusals{"grime_sync"};

}  // namespace

bool AdmitValue(float value, uint8_t senderPeerSlot) {
    const char* why = nullptr;
    if (!std::isfinite(value)) why = "a value that is not a number";
    else if (value < 0.f && value != coop::net::kGrimeGone) why = "a value below the ended mark";
    else if (value > coop::net::kGrimeDirtMax) why = "a value above the dirt the game holds";
    if (!why) return true;
    g_refusals.Note(senderPeerSlot, why, coop::net::RefusalTally::Clock::now());
    return false;
}

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
        // And every key the index does not hold live, at the lowest of its floor and its parked decals: a
        // decal ended here -- zero -- or streamed out, which the joiner has from its own copy of the save and
        // must see as this world has it.
        std::unordered_map<std::wstring, float> away(floor.begin(), floor.end());
        for (const auto& kv : g_parked.Get()) {
            const auto ins = away.emplace(kv.second.key, kv.second.value);
            if (!ins.second && kv.second.value < ins.first->second) ins.first->second = kv.second.value;
        }
        for (const auto& kv : away) {
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
    g_refusals.Flush(coop::net::RefusalTally::Clock::now());
    if (!G::EnsureResolved()) return;
    RegisterWithScanHub();  // safety net for any order where Tick precedes Install
    // The two lowering verbs' watches and the decals' end of play, each asked again until it takes. A
    // name watch matches the verb on any class; the pre half keeps only a grime decal.
    if (!g_cleanWatched) g_cleanWatched = sg::WatchName(kRowClean, kTagGrimeClean, &OnLowerPre, &OnLowerPost);
    if (!g_fixWatched) g_fixWatched = sg::WatchName(kRowWallFixerFix, kTagGrimeFix, &OnLowerPre, &OnLowerPost);
    if (!g_endSubscribed && DS::Install()) g_endSubscribed = DS::SubscribeClass(L"grime_C", &OnGrimeEnd);
    if (!g_replayWatched) g_replayWatched = sg::WatchName(kRowLoadObjects, kTagGrimeReplay, &OnLoadObjectsPre, nullptr);
    sg::ResolvePendingNames();
}

void IndexNow(void* decal) {
    // The spawn seam's hand-over: the decal is named and indexed as it is born, so its end is sent however soon it comes.
    if (!decal || !G::IsGrime(decal) || !IndexCurrent()) return;
    CachedKey ck;
    if (!KeysOf(decal, ck.key, ck.exact)) return;
    ck.idx = R::InternalIndexOf(decal);
    ck.serial = R::AllocateSlotSerial(ck.idx);
    ck.provisional = true;
    {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        const auto twin = g_byKey.find(ck.key);
        if (twin != g_byKey.end() && twin->second.actor != decal && Holds(twin->second)) {
            ++g_indexNowTwins;   // a live twin holds the key: the first stays, as a pass keeps the last it finds
            return;
        }
        g_byKey[ck.key] = Ref{decal, ck.idx, ck.serial};
    }
    if (g_scanIsFull) g_scanNextCache.insert_or_assign(decal, ck);   // a full pass in flight keeps it through its swap
    g_posKeyByActor.insert_or_assign(decal, std::move(ck));
}

void ForgetDecal(void* actor) {
    if (!actor) return;
    // The index holds a decal under the key its cache entry names: no walk of the index.
    const auto cached = g_posKeyByActor.find(actor);
    if (cached != g_posKeyByActor.end()) {
        std::lock_guard<std::mutex> lk(g_indexMutex);
        const auto it = g_byKey.find(cached->second.key);
        if (it != g_byKey.end() && it->second.actor == actor) g_byKey.erase(it);
    }
    g_posKeyByActor.erase(actor);
    g_scanNextCache.erase(actor);
}

void* DecalOfKey(const std::wstring& key) { return ResolveFast(key); }

bool HostValueOf(const std::wstring& key, void* decal, float& out) {
    float process = 0.f;
    if (!decal || !G::ReadProcess(decal, process)) return false;
    const auto& floor = Floor();
    const auto it = floor.find(key);
    out = (it != floor.end() && it->second < process) ? it->second : process;
    return true;
}

void GrantFall(coop::net::Session& session, const std::wstring& key, void* decal, float value, uint8_t askerSlot,
               bool raiseAsker) {
    Lower(key, value);
    ApplyResolved(decal, key, value, "a granted ask", /*lowerOnly=*/true);
    // The decal is lowered; the others hear the fall as a peer's own live fall, the asker only when it was cut.
    Send(key, value, askerSlot);
    if (raiseAsker) {
        coop::net::KeyedScalarPayload p{};
        WireKeyFromString(key, p.key);
        p.value = value;
        p.adopt = 1;
        session.SendReliableToSlot(askerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
    }
}

void GrantEnd(void* decal) {
    // Not ApplyResolved's destroy: that one forgets the decal first, so its end of play sends nothing. Here the end of
    // play is the send (OnGrimeEnd), the one that tells the asker its held decal is gone.
    E::DestroyActor(decal);
}

void AnswerAsker(coop::net::Session& session, const std::wstring& key, float hostValue, uint8_t askerSlot) {
    coop::net::KeyedScalarPayload p{};
    WireKeyFromString(key, p.key);
    p.value = hostValue;
    p.adopt = 1;
    session.SendReliableToSlot(askerSlot, coop::net::ReliableKind::GrimeState, &p, sizeof(p));
}

Tally Tallies() {
    return Tally{g_replays.load(std::memory_order_relaxed), g_passes.load(std::memory_order_relaxed),
                 g_lowered.load(std::memory_order_relaxed), g_endsSent.load(std::memory_order_relaxed), g_passGen.load(std::memory_order_relaxed)};
}

std::wstring DebugPosKeyForActor(void* actor) {
    // The dev drill's parity probe: the real quantiser, cache-independent.
    return actor ? PosKey(actor) : std::wstring();
}

}  // namespace coop::grime_sync
