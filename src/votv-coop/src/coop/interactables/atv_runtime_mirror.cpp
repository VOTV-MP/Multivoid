// coop/interactables/atv_runtime_mirror.cpp -- a client's hold on the ATVs the host announced (declared in
// coop/interactables/atv_sync.h). The host announces each ATV it holds under its game Key. This client holds
// exactly one actor per Key: the one its world already carries (the save's copy, found in the lane's map or the
// game's own key map) is adopted, and a native ATV carrying the Key, physics on so it is grabbable, is spawned
// only when the world holds none. The copy under a Key goes when the host's does, and the host's complete set
// removes the ATVs its world no longer holds (OnAtvSetComplete).
// An arrival landing while this world is not ready waits in the park (atv_spawn_park.h). The map stays
// atv_sync.cpp's: this half asks it to adopt or drop a row (atv_sync_internal.h).

#include "coop/interactables/atv_sync.h"
#include "atv_spawn_park.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_index_internal.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/session/world_load_episode.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/keyed_objects.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace coop::atv_sync {
namespace {

namespace R = ue_wrap::reflection;
namespace A = ue_wrap::atv;
using ue_wrap::FVector;
using ue_wrap::FRotator;
using coop::net::StringFromWireKey;

// One session's arrival state; ResetArrivalState drops it all at a disconnect.
// The keys the AtvSpawns named since the previous edge: the set the next edge closes. The most keys one set, or the
// session's record, holds is spawn_park::kCap, far above the ATVs of a world.
std::unordered_set<std::wstring> g_setKeys;
uint32_t g_setGen = 0;   // the world generation g_setKeys was named in (0 = none yet)
// Every key the session's AtvSpawns named, not cleared at an edge.
std::unordered_set<std::wstring> g_receivedKeys;
bool g_setOverflowed = false;   // g_setKeys passed the cap: its edge removes nothing
bool g_saidOverflow = false;
// The host's set, closed by its edge and waiting for this world to be ready to take it.
struct PendingSet {
    std::unordered_set<std::wstring> keys;
    uint32_t count = 0;      // what the edge declared
    uint32_t worldGen = 0;   // the world the edge arrived in
    uint64_t nextWalkMs = 0; // after a walk that met a loading ATV, the next walk waits until then
    bool     present = false;
} g_pending;
uint32_t g_adoptedEver = 0;     // announces answered by a copy the world already held
uint32_t g_spawnedEver = 0;     // mirrors this lane spawned
uint32_t g_destroysApplied = 0; // actors destroyed for an AtvDestroy
uint32_t g_setEdgesApplied = 0;
uint32_t g_deliberateParks = 0; // the spawn drill's park arm
bool g_parkedFirst = false;     // the park arm's latch, once per session

std::wstring WireClassNameToString(const coop::net::WireClassName& in) {
    std::wstring s;
    const uint8_t n = in.len <= sizeof(in.data) ? in.len : static_cast<uint8_t>(sizeof(in.data));
    s.reserve(n);
    for (uint8_t i = 0; i < n; ++i) s.push_back(static_cast<wchar_t>(static_cast<unsigned char>(in.data[i])));
    return s;
}

// The one gate every arrival waits on, AtvSpawn, AtvDestroy and the set's edge alike: a gameplay world whose save
// replay has returned and whose ATV index is current, with the wrapper, the name library and the game's key map
// all answering. Null when open, else what is missing.
// Divergence from MTA, whose joiner takes the join packet only once its game is loaded and raises a protocol error
// otherwise (reference/mtasa-blue/Client/mods/deathmatch/logic/CPacketHandler.cpp:338-342): the host's arrivals can
// precede this world's readiness, since the save loads beside them, so they wait in the park.
const char* GateClosedBecause() {
    if (ue_wrap::world_identity::CurrentWorldKind() != ue_wrap::world_identity::WorldKind::Gameplay ||
        !IndexCurrent() || !coop::world_load_episode::HasQuiesced())
        return "the receiving world is not ready";
    if (!A::EnsureResolved()) return "the ATV wrapper is not resolved";
    if (!ue_wrap::fname_utils::Resolved()) return "the name library is not resolved";
    if (!ue_wrap::keyed_objects::Available()) return "the game's key map cannot be asked";
    return nullptr;
}

// The host named this key: it is in the next set and in the session's record. Both are capped, and a set past the
// cap is marked, so its edge removes nothing rather than judge by a part of it. The open set is every key named
// since the last edge in this world: a withheld snapshot and its retries add up, and a key the host lost without
// an AtvDestroy stays named, which fails toward not removing.
void NoteNamed(const std::wstring& key) {
    // Generation() only loads; the refresh runs inside the accessors, so ask one first or a batch can straddle
    // a change this tick already holds.
    (void)ue_wrap::world_identity::CurrentWorldKind();
    // Keys named in a world since left are not this world's set.
    if (const uint32_t gen = ue_wrap::world_identity::Generation(); gen != g_setGen) {
        g_setKeys.clear();
        g_setOverflowed = false;
        g_setGen = gen;
    }
    bool over = false;
    if (!g_setOverflowed && !g_setKeys.count(key)) {
        if (g_setKeys.size() >= spawn_park::kCap) { g_setOverflowed = true; over = true; }
        else g_setKeys.insert(key);
    }
    if (!g_receivedKeys.count(key)) {
        if (g_receivedKeys.size() >= spawn_park::kCap) over = true;
        else g_receivedKeys.insert(key);
    }
    if (over && !g_saidOverflow) {
        g_saidOverflow = true;
        UE_LOGE("atv: the host named more than %zu ATV keys -- the set in flight removes nothing and the "
                "record of received keys stops growing (said once)", spawn_park::kCap);
    }
}

// The ATV the game's own key map holds under `key`, accepted only as a live ATV of this world that really
// carries that key: the map answers for any class and never checks a world, and it can hold a default object.
void* KeyMapActor(const std::wstring& key) {
    void* actor = ue_wrap::keyed_objects::Resolve(key.c_str());
    if (!actor || !A::IsAtv(actor) || R::NameStartsWith(R::NameOf(actor), L"Default__")) return nullptr;
    if (A::GetKeyString(actor) != key) return nullptr;
    return detail::LiveInThisWorld(actor, R::InternalIndexOf(actor)) ? actor : nullptr;
}

void Adopted(const std::wstring& key, void* actor, const char* via) {
    spawn_park::Discard(key);
    ++g_adoptedEver;
    UE_LOGI("atv: adopted the world's copy key='%ls' actor=%p (%s)", key.c_str(), actor, via);
}

}  // namespace

void ResetArrivalState() {
    g_setKeys.clear();
    g_setGen = 0;
    g_receivedKeys.clear();
    g_setOverflowed = false;
    g_saidOverflow = false;
    g_pending = PendingSet{};
    g_adoptedEver = g_spawnedEver = g_destroysApplied = g_setEdgesApplied = g_deliberateParks = 0;
    g_parkedFirst = false;
}

void OnAtvSpawn(const coop::net::AtvSpawnPayload& payload) {
    // From the host alone: the host's receive edge refuses a client's copy (client_authorship.h).
    auto* s = LaneSession();
    if (!s || !s->connected()) return;
    if (!detail::WireKeyNamesAnAtv(payload.key, "client")) return;
    const std::wstring key = StringFromWireKey(payload.key);
    // The host named this key whatever becomes of the payload, so the set counts it here, before the checks
    // that may drop it and before the gate that may park it. Every arrival off the wire counts, a key still
    // parked from an earlier replay too; the park's hand-back enters at ResolveSpawn and is not counted again.
    NoteNamed(key);
    ResolveSpawn(key, payload);
}

void ResolveSpawn(const std::wstring& key, const coop::net::AtvSpawnPayload& payload) {
    // The payload is checked before any wait: a malformed spawn is dropped, only a sound one is deferred.
    if (!std::isfinite(payload.x) || !std::isfinite(payload.y) || !std::isfinite(payload.z) ||
        !std::isfinite(payload.pitch) || !std::isfinite(payload.yaw) || !std::isfinite(payload.roll)) {
        UE_LOGW("atv: OnAtvSpawn non-finite pose key='%ls' -- dropping", key.c_str());
        spawn_park::Discard(key);  // a malformed spawn is final
        return;
    }
    std::wstring className = WireClassNameToString(payload.className);
    if (className.empty()) {
        UE_LOGW("atv: OnAtvSpawn empty className key='%ls' -- dropping", key.c_str());
        spawn_park::Discard(key);
        return;
    }
    if (const char* why = GateClosedBecause()) {
        spawn_park::Park(key, payload, why);
        return;
    }
    // Resolve by key: the copy this world already holds is the host's ATV, so nothing is spawned beside it.
    // The drill's red arm skips both steps, so a mirror always spawns.
    static const bool s_adoptBreak = coop::config::ResolveFlag(coop::config_registry::rows::atv_adopt_break);
    if (!s_adoptBreak) {
        // (a) The lane's own row, live in this world: a mirror this lane spawned is a duplicate announce; the
        // save's copy, filed by the hub, is adopted.
        if (const AtvEntry* known = FindEntry(key)) {
            if (detail::LiveInThisWorld(known->actor, known->idx)) {
                if (known->isClientSpawnedMirror) { spawn_park::Discard(key); return; }
                Adopted(key, known->actor, "row");
                return;
            }
            EraseEntry(key);   // a dead row is not "already spawned"
        }
        // (b) The game's key map, which holds the save's copy once its load has run even where the hub's pass has
        // not filed it yet. One linear lookup, only on a row miss.
        if (void* held = KeyMapActor(key)) {
            detail::AdoptNative(key, held);
            Adopted(key, held, "key map");
            return;
        }
    }
    // (c) Nothing here under the key. The ATV spawn drill's park arm parks the first arrival that would spawn,
    // once per session, as if the world were not ready, so the park's hand-back is what spawns it. A hand-back
    // of its own is let through by the latch; one of a park the world forced is parked once more.
    static const bool s_parkFirst = coop::config::ResolveFlag(coop::config_registry::rows::atv_park_first_spawn);
    if (s_parkFirst && !g_parkedFirst) {
        g_parkedFirst = true;
        ++g_deliberateParks;
        spawn_park::Park(key, payload, "the drill parks the first spawn", /*say*/ true);
        if (coop::atv_drill_events::On()) spawn_park::HoldForDrill(key);   // the ATV gate drill's joiner step
        return;
    }
    const FVector loc{ payload.x, payload.y, payload.z };
    const FRotator rot{ payload.pitch, payload.yaw, payload.roll };
    A::SpawnFail why = A::SpawnFail::None;
    std::wstring bornKey;
    // physics LEFT ON -- a native idle grabbable ATV
    void* spawned = A::SpawnMirror(className, loc, rot, key, &why, &bornKey);
    if (!spawned) {
        // In a world that reads ready, a refused spawn is the class, the key or the engine saying no: final,
        // never a mirror under a key of its own making, and said once per key (every replay asks again).
        // The wrapper reports the step and logs nothing; a key mismatch names both keys.
        if (detail::FirstSaying(L"spawn-failed|", key)) {
            if (why == A::SpawnFail::KeyMismatch)
                UE_LOGW("atv: OnAtvSpawn SpawnMirror failed key='%ls' class='%ls': %s -- born with key '%ls', "
                        "not the requested '%ls' -- dropped (said once per key)",
                        key.c_str(), className.c_str(), A::SpawnFailText(why), bornKey.c_str(), key.c_str());
            else
                UE_LOGW("atv: OnAtvSpawn SpawnMirror failed key='%ls' class='%ls': %s -- dropped (said once per key)",
                        key.c_str(), className.c_str(), A::SpawnFailText(why));
        }
        spawn_park::Discard(key);
        return;
    }
    spawn_park::Discard(key);
    AdoptClientMirror(key, spawned);
    ++g_spawnedEver;
    UE_LOGI("atv: spawned ATV mirror key='%ls' class='%ls' actor=%p loc=(%.0f, %.0f, %.0f)",
            key.c_str(), className.c_str(), spawned, loc.X, loc.Y, loc.Z);
}

size_t ParkPending() { return spawn_park::Pending(); }
uint32_t AdoptedEver() { return g_adoptedEver; }
uint32_t SpawnedEver() { return g_spawnedEver; }
uint32_t DestroysApplied() { return g_destroysApplied; }
uint32_t SetEdgesApplied() { return g_setEdgesApplied; }
uint32_t DeliberateParks() { return g_deliberateParks; }
std::vector<std::wstring> ReceivedSpawnKeys() {
    return std::vector<std::wstring>(g_receivedKeys.begin(), g_receivedKeys.end());
}

void OnAtvDestroy(const coop::net::AtvDestroyPayload& payload) {
    if (!LaneSession()) return;  // from the host alone: its receive edge refuses a client's copy
    if (!detail::WireKeyNamesAnAtv(payload.key, "client")) return;
    const std::wstring key = StringFromWireKey(payload.key);
    // The destroy is the host's last word on the key: a set still open no longer names it.
    g_setKeys.erase(key);
    if (const char* why = GateClosedBecause()) {
        spawn_park::ParkDestroy(key, why);   // the destroy is the host's last word on the key: it waits, not drops
        return;
    }
    spawn_park::Discard(key);  // a destroy ends a spawn still waiting
    // The copy standing here goes, whether it is a mirror this lane spawned or a native one the save placed: the
    // row's live actor, else the game's key map's.
    void* actor = nullptr;
    const AtvEntry* row = FindEntry(key);
    if (row && detail::LiveInThisWorld(row->actor, row->idx)) actor = row->actor;
    else actor = KeyMapActor(key);
    if (!row && !actor) {
        // Nothing held under this key and nothing to destroy: a spawn that never landed here, or one a
        // pass already pruned.
        if (detail::FirstSaying(L"destroy-unheld|", key))
            UE_LOGI("atv: AtvDestroy key='%ls' -- no row held, nothing to destroy (said once per key)",
                    key.c_str());
        return;
    }
    const bool destroyed = actor && A::DestroyMirror(actor);
    if (destroyed) ++g_destroysApplied;
    EraseEntry(key);
    UE_LOGI("atv: ATV key='%ls' %s", key.c_str(),
            destroyed ? "destroyed (the host's is gone)" : "row dropped, no live actor was destroyed");
}

void OnAtvSetComplete(const coop::net::AtvSetCompletePayload& payload) {
    auto* s = LaneSession();
    if (!s || !s->connected()) return;
    // The edge closes the set before the gate, not after: the AtvSpawns it counts are already named, parked or
    // not, and the arrivals after it start the next set. Divergence from MTA, which builds a joiner's world from
    // the host's packets alone and counts an entity batch up front
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CMapManager.cpp:195, SendMapInformation, and
    // reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:90-94): this client first
    // loads the host's save, so the host's complete set, closed by an edge carrying its count, lets it drop what
    // the save holds and the host does not. The set is N reliable messages, so its count trails the batch and the
    // host can withhold the edge after a partial send, which MTA's one counted packet never needs.
    // Only keys named in this world are its set: those of a world since left count as none. Refreshed first, as
    // in NoteNamed.
    (void)ue_wrap::world_identity::CurrentWorldKind();
    const bool current = g_setGen == ue_wrap::world_identity::Generation();
    const bool otherWorld = !current && g_setGen != 0;
    std::unordered_set<std::wstring> keys;
    if (current) keys = std::move(g_setKeys);
    g_setKeys.clear();
    const bool overflowed = current && g_setOverflowed;
    g_setOverflowed = false;
    if (overflowed) {
        UE_LOGW("atv: the host's set of %u is not applied -- more keys arrived than a set holds", payload.count);
        return;
    }
    if (keys.size() < payload.count) {
        if (otherWorld) {
            UE_LOGI("atv: the host's set of %u is not applied -- its AtvSpawns were named in a world since left; "
                    "the next replay brings a fresh one", payload.count);
            return;
        }
        UE_LOGE("atv: the host's set declares %u ATV(s) but %zu AtvSpawn key(s) arrived ahead of it -- the lane is "
                "ordered, so this is a defect; nothing is removed", payload.count, keys.size());
        return;
    }
    g_pending.keys = std::move(keys);
    g_pending.count = payload.count;
    g_pending.worldGen = ue_wrap::world_identity::Generation();
    g_pending.present = true;   // a later edge replaces an older pending set
    ApplyPendingSet();
}

void ApplyPendingSet() {
    if (!g_pending.present) return;
    if (g_pending.worldGen != ue_wrap::world_identity::Generation()) {
        UE_LOGI("atv: the host's set of %u is dropped -- its world was left; the next replay brings a fresh one",
                g_pending.count);
        g_pending = PendingSet{};
        return;
    }
    if (g_setOverflowed) {
        // The keys named since the edge are incomplete, and a stranger is judged against them.
        UE_LOGW("atv: the host's set of %u is dropped -- more keys arrived since its edge than a set holds; "
                "nothing is removed", g_pending.count);
        g_pending = PendingSet{};
        return;
    }
    if (GateClosedBecause()) return;
    if (NowMs() < g_pending.nextWalkMs) return;
    // Collected during the walk and destroyed after it: the object index's callback must not destroy.
    struct Victim { void* actor; std::wstring key; };
    struct Walk {
        const std::unordered_set<std::wstring>* set;
        const std::unordered_set<std::wstring>* next;   // named since the edge: the next set's, not a stranger
        std::vector<Victim> victims;
    } walk{ &g_pending.keys, &g_setKeys, {} };
    const size_t loading = ForEachAnnounceableAtv([](void* ctx, void* actor, const std::wstring& key) {
        auto* w = static_cast<Walk*>(ctx);
        if (!w->set->count(key) && !w->next->count(key)) w->victims.push_back(Victim{ actor, key });
    }, &walk);
    // An ATV still loading is not judged: the set stays pending and is judged again half a second later (DrainReady
    // calls every tick, the walk is not a per-tick cost), with nothing destroyed from this walk. A world change drops
    // it (above).
    if (loading > 0) {
        g_pending.nextWalkMs = NowMs() + 500;
        return;
    }
    // The drill's red arm applies the set without removing anything.
    static const bool s_setBreak = coop::config::ResolveFlag(coop::config_registry::rows::atv_set_break);
    uint32_t removed = 0;
    for (const Victim& v : walk.victims) {
        if (s_setBreak || !A::DestroyMirror(v.actor)) continue;
        if (const AtvEntry* row = FindEntry(v.key); row && row->actor == v.actor) EraseEntry(v.key);
        ++g_destroysApplied;
        ++removed;
        UE_LOGI("atv: the host's set does not name '%ls' -- removed", v.key.c_str());
    }
    UE_LOGI("atv: the host's set of %u applied (removed %u)", g_pending.count, removed);
    ++g_setEdgesApplied;
    g_pending = PendingSet{};
}

}  // namespace coop::atv_sync
