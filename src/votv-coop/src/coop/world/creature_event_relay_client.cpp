// coop/world/creature_event_relay_client.cpp -- the client's half of coop/world/creature_event_relay.h: a creature's
// target from the host, its mirror's bind and end, and the one end rule that retracts what was applied.
//
// A client's activeEvents is the host's relayed creature events, each keyed to its eid, PLUS the client's own
// peer-local events; the relay only adds and removes its own keyed registrations (setEvent with the mirror as the
// context, or the gamemode as the context when the mirror is gone), and never sets the count.

#include "coop/world/creature_event_relay.h"
#include "creature_event_relay_internal.h"  // co-located private header (src tree, not include/)

#include "coop/element/death_seam.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::creature_event_relay::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace WI = ue_wrap::world_identity;
namespace EL = coop::element;

// One creature's state on this client. "Bound" means `actor` is set: the mirror was installed. `target` is the
// host's count, `applied` what this relay has registered in the gamemode's count for the creature.
struct Entry {
    int      target = 0;
    int      applied = 0;
    void*    actor = nullptr;
    int32_t  actorIndex = -1;
    int32_t  actorSerial = 0;
    uint32_t generation = 0;     // WI::Generation() when the entry was made
    void*    world = nullptr;    // WI::WorldOf(actor) at the bind: an address to compare, never dereferenced
};
std::unordered_map<EL::ElementId, Entry> g_entries;
std::unordered_set<EL::ElementId> g_refusalLogged;  // creatures whose refused call was said once; at most kMaxEntries
uint32_t g_sweptGeneration = 0;

// The setEvent calls a per-viewer mirror made itself and ran, by eid; at most kMaxEntries creatures. They outlive
// the mirror, so a count read after its destroy still pairs; the disconnect drops them.
struct ViewerCalls {
    uint32_t trues  = 0;
    uint32_t falses = 0;
};
std::unordered_map<EL::ElementId, ViewerCalls> g_viewerCalls;

void CountViewerCall(void* ctx, bool active) {
    const EL::ElementId eid = NpcEidOf(ctx, /*mirror=*/true);
    if (eid == EL::kInvalidId) return;
    if (g_viewerCalls.size() >= kMaxEntries && g_viewerCalls.find(eid) == g_viewerCalls.end()) return;
    ViewerCalls& v = g_viewerCalls[eid];
    ++(active ? v.trues : v.falses);
}

bool Bound(const Entry& e) { return e.actor != nullptr; }

bool ActorLive(const Entry& e) {
    return R::IsLiveByIndex(e.actor, e.actorIndex) && R::SlotSerial(e.actorIndex) == e.actorSerial;
}

// A world change took the gamemode and its count with it: every entry of another world goes without a `false`.
void SweepOtherWorlds() {
    const uint32_t gen = WI::Generation();
    if (gen == g_sweptGeneration) return;
    g_sweptGeneration = gen;
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.generation != gen) it = g_entries.erase(it);
        else ++it;
    }
    g_refusalLogged.clear();  // every mirror it named went with its world
}

// `n` registrations taken off the gamemode's count, with `ctx` as the context.
void Retract(void* ctx, int n) {
    for (int i = 0; i < n; ++i)
        if (!Call(false, ctx)) break;
}

// One registration more or fewer for the entry's mirror; false when the call could not be made.
bool Step(Entry& e, bool active) {
    if (!Call(active, e.actor)) return false;
    e.applied += active ? 1 : -1;
    return true;
}

// The one end rule. The entry's applied count is retracted with the gamemode as the context when its world is
// the running one (the mirror is gone or going, and is never passed to Blueprint again); otherwise the count died
// with its world and is dropped without a call. The entry is erased.
void EndEntry(EL::ElementId eid) {
    const auto it = g_entries.find(eid);
    if (it == g_entries.end()) return;
    const Entry e = it->second;
    g_entries.erase(it);
    g_refusalLogged.erase(eid);  // a reused eid says its refusal again
    if (e.applied <= 0) return;
    // The generation and the world kind are memoised and can lag a change by 100 ms, so the gamemode itself is the
    // guard: it must answer and belong to the world the entry was bound in.
    void* gm = nullptr;
    if (e.generation == WI::Generation() && WI::CurrentWorldKind() == WI::WorldKind::Gameplay)
        gm = ue_wrap::world_singleton::Gamemode();
    if (gm && WI::WorldOf(gm) == e.world) {
        Retract(gm, e.applied);
        UE_LOGI("creature_event_relay: eid=%u ended, %d retracted", eid, e.applied);
    } else {
        UE_LOGI("creature_event_relay: eid=%u ended in another world, %d dropped", eid, e.applied);
    }
}

// Bring the entry's applied count to its target. An entry of another world is erased; a bound entry whose actor
// is gone is ended; an unbound entry at target 0 is erased.
void Reconcile(EL::ElementId eid) {
    auto it = g_entries.find(eid);
    if (it == g_entries.end()) return;
    Entry& e = it->second;
    if (e.generation != WI::Generation()) {
        g_entries.erase(it);
        return;
    }
    if (!Bound(e)) {
        if (e.target == 0) g_entries.erase(it);
        return;
    }
    if (!ActorLive(e)) {
        EndEntry(eid);
        return;
    }
    if (!Ready()) return;  // setEvent, the event registry or lib_C's default object unresolved; the next count reconciles
    while (e.applied < e.target && Step(e, true)) {}
    while (e.applied > e.target && Step(e, false)) {}
}

}  // namespace

// A call is cancelled only when the relay holds a bound entry for this very actor: a mirror the relay never bound
// (no event registry yet, a full table, a per-viewer class) counts its own, and runs natively; a per-viewer
// mirror's own calls are counted for the drill. The PRE is the suppression at the source, as MTA disables a local
// writer where it writes (reference/mtasa-blue/Client/multiplayer_sa/CMultiplayerSA.cpp:854-858).
bool ClientPre(void* ctx, bool active) {
    if (!ctx || ctx == g_applyingCtx) return false;
    if (IsPerViewer(ctx)) {
        CountViewerCall(ctx, active);
        return false;
    }
    if (g_entries.empty()) return false;
    const EL::ElementId eid = NpcEidOf(ctx, /*mirror=*/true);
    if (eid == EL::kInvalidId) return false;
    const auto it = g_entries.find(eid);
    if (it == g_entries.end() || !Bound(it->second) || it->second.actor != ctx ||
        it->second.generation != WI::Generation())
        return false;
    if (g_refusalLogged.size() < kMaxEntries && g_refusalLogged.insert(eid).second)
        UE_LOGI("creature_event_relay: mirror eid=%u's own setEvent(%d) refused (the host's count is relayed)", eid,
                active ? 1 : 0);
    return true;
}

void ClientOnReliable(const coop::net::CreatureEventActivePayload& payload) {
    if (!ActiveSession(coop::net::Role::Client)) return;
    SweepOtherWorlds();
    const EL::ElementId eid = static_cast<EL::ElementId>(payload.elementId);
    auto it = g_entries.find(eid);
    if (it == g_entries.end()) {
        if (payload.count == 0) return;  // the zero a destroy sends after its EntityDestroy
        // Diverges from MTA dropping data for an unknown element
        // (reference/mtasa-blue/Client/mods/deathmatch/logic/rpc/CRPCFunctions.cpp:156-167): here a count may precede
        // its creature's spawn, so the target is kept.
        if (g_entries.size() >= kMaxEntries) {
            UE_LOGW("creature_event_relay: eid=%u count=%u refused -- %zu creatures already hold a target", eid,
                    static_cast<unsigned>(payload.count), kMaxEntries);
            return;
        }
        it = g_entries.emplace(eid, Entry{}).first;
        it->second.generation = WI::Generation();
    }
    it->second.target = payload.count;
    Reconcile(eid);
}

void ClientOnMirrorBound(EL::ElementId eid, void* actor) {
    if (!actor || !ActiveSession(coop::net::Role::Client) || IsPerViewer(actor)) return;
    SweepOtherWorlds();
    const int32_t index = R::InternalIndexOf(actor);
    const int32_t serial = R::AllocateSlotSerial(index);
    auto it = g_entries.find(eid);
    if (it != g_entries.end() && it->second.actor == actor && it->second.actorIndex == index &&
        it->second.actorSerial == serial)
        return;
    // No entry is made when the bind cannot apply (each WARN says so): the PRE then leaves this mirror native, so its
    // own true keeps its own false, the mirror's count stays paired by its Blueprint, and the host's count for it is
    // not applied. MTA has no such fallback, its server always authors element data
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1005, SetElementData).
    if (!Ready()) {
        UE_LOGW("creature_event_relay: eid=%u bind before setEvent, the event registry and lib_C's default object "
                "resolved -- nothing applied, the mirror counts its own", eid);
        return;
    }
    if (it == g_entries.end() && g_entries.size() >= kMaxEntries) {
        UE_LOGW("creature_event_relay: eid=%u bind refused -- %zu creatures already hold an entry, the mirror counts "
                "its own", eid, kMaxEntries);
        return;
    }
    // The registrations the actor made as its own creature before it was installed: a born mirror's BeginPlay
    // `true`, an adopted twin's. The game's own array is the only record of them.
    int copies = 0;
    if (!CopiesInRegistry(actor, copies)) {
        UE_LOGW("creature_event_relay: eid=%u bind without a readable event registry -- nothing applied", eid);
        return;
    }
    // An entry bound to another actor of this eid ended unheard: its count is retracted first, and the host's
    // target stays.
    int target = 0;
    if (it != g_entries.end()) {
        target = it->second.target;
        if (Bound(it->second)) EndEntry(eid);
        else g_entries.erase(it);
    }
    // An adopted twin registered before it was ours, so its registrations are undone after the fact, which MTA's
    // client never needs: it runs no server element's logic before receiving it.
    Retract(actor, copies);
    Entry& e = g_entries[eid];
    e.target = target;
    e.applied = 0;
    e.actor = actor;
    e.actorIndex = index;
    e.actorSerial = serial;
    e.generation = WI::Generation();
    e.world = WI::WorldOf(actor);
    Reconcile(eid);
}

void ClientOnNpcEnd(const EL::death_seam::Death& death) {
    SweepOtherWorlds();
    const auto it = g_entries.find(death.eid);
    if (it == g_entries.end()) return;
    const Entry& e = it->second;
    if (e.actor == death.actor && e.actorIndex == death.actorIndex && e.actorSerial == death.actorSerial)
        EndEntry(death.eid);
}

// ---- the killer wisp's per-viewer check -------------------------------------------------------------------------------
//
// killerwisp_C's BeginPlay keeps a loop on a 3 s Delay: `rendered` = player 0 within 5000 and not avoidPlayer, then
// lib_C::setEvent(rendered, false, self), the per-viewer event its viewer's event music follows. A Delay advances only
// while the actor ticks, and a client's mirror is parked with its actor tick off (the tick also moves player 0 in a
// grab and runs the capture), so the loop stopped at its first pass: the host heard the wisp's music and a client never
// did (VOTV-MP/Multivoid#193). The client runs that one check for each of its bound killer wisp mirrors (the killer lamb
// is one too), with this player for player 0, and calls setEvent when the answer changes. The mirror's own end runs
// its setEvent(false) as the game's does.
namespace {

constexpr uint64_t kWispCheckMs = 3000;   // the Blueprint's Delay
constexpr float    kWispViewCm  = 5000.f;  // its distance to player 0

struct WispView {
    void* actor = nullptr;
    bool  rendered = false;
};
std::unordered_map<EL::ElementId, WispView> g_wispViews;
uint64_t g_nextWispCheck = 0;

bool ReadBit(void* obj, const wchar_t* name, bool& out, int32_t* offOut = nullptr, uint8_t* maskOut = nullptr) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    out = (static_cast<const uint8_t*>(obj)[off] & mask) != 0;
    if (offOut) *offOut = off;
    if (maskOut) *maskOut = mask;
    return true;
}

void CheckWisp(void* wisp, void* me, const ue_wrap::FVector& at) {
    const EL::ElementId eid = NpcEidOf(wisp, /*mirror=*/true);
    if (eid == EL::kInvalidId) return;
    bool avoid = false, was = false;
    int32_t off = -1;
    uint8_t mask = 0;
    ReadBit(wisp, L"avoidPlayer", avoid);
    if (!ReadBit(wisp, L"rendered", was, &off, &mask)) return;
    ue_wrap::FVector w{};
    if (!me || !ue_wrap::engine::TryGetActorLocation(wisp, w)) return;
    const float dx = w.X - at.X, dy = w.Y - at.Y, dz = w.Z - at.Z;
    const bool rendered = dx * dx + dy * dy + dz * dz <= kWispViewCm * kWispViewCm && !avoid;
    auto it = g_wispViews.find(eid);
    if (it == g_wispViews.end() || it->second.actor != wisp) {
        // First sight: the Blueprint's first pass already ran on this mirror at its BeginPlay and set `rendered`.
        it = g_wispViews.insert_or_assign(eid, WispView{wisp, was}).first;
    }
    if (it->second.rendered == rendered) return;
    uint8_t* const b = static_cast<uint8_t*>(wisp) + off;
    *b = rendered ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    if (Call(rendered, wisp)) {
        it->second.rendered = rendered;
        UE_LOGI("creature_event_relay: killer wisp mirror eid=%u %s this player -- setEvent(%d)", eid,
                rendered ? "near" : "away from", rendered ? 1 : 0);
    }
}

}  // namespace

void ClientTickWisps(uint64_t nowMs) {
    if (!ActiveSession(coop::net::Role::Client) || nowMs < g_nextWispCheck || !Ready()) return;
    g_nextWispCheck = nowMs + kWispCheckMs;
    void* const me = coop::players::Registry::Get().Local();
    ue_wrap::FVector at{};
    if (!me || !ue_wrap::engine::TryGetActorLocation(me, at)) return;
    std::vector<void*> wisps;
    for (const wchar_t* cls : {L"killerwisp_C", L"killerlamb_C"})
        if (void* const c = ue_wrap::object_index::ClassByName(cls))
            ue_wrap::object_index::ForEachInstance(c, [](void* ctx, void* o, int32_t) {
                if (o && R::IsLive(o)) static_cast<std::vector<void*>*>(ctx)->push_back(o);
            }, &wisps);
    for (void* w : wisps) CheckWisp(w, me, at);
    for (auto it = g_wispViews.begin(); it != g_wispViews.end();)
        it = R::IsLive(it->second.actor) ? std::next(it) : g_wispViews.erase(it);
}

void ClientOnDisconnect() {
    g_wispViews.clear();
    g_nextWispCheck = 0;
    // The retraction runs only while the entries' world is the running one (EndEntry's own test); on a
    // quit-to-menu the world is going and nothing is called into it. Only the relay's own registrations are retracted:
    // diverges from MTA's wholesale teardown, ~CClientGame (reference/mtasa-blue/Client/mods/deathmatch/logic/
    // CClientGame.cpp:417), which deletes the client manager at :559 and the element managers under it
    // (CClientManager.cpp:66), for the client keeps its own world and its peer-local events.
    std::vector<EL::ElementId> ids;
    ids.reserve(g_entries.size());
    for (const auto& kv : g_entries) ids.push_back(kv.first);
    for (const EL::ElementId id : ids) EndEntry(id);
    g_entries.clear();
    g_refusalLogged.clear();
    g_viewerCalls.clear();
    g_sweptGeneration = 0;
}

int ClientTargetFor(EL::ElementId eid) {
    const auto it = g_entries.find(eid);
    return it == g_entries.end() ? -1 : it->second.target;
}

int ClientAppliedFor(EL::ElementId eid) {
    const auto it = g_entries.find(eid);
    return it == g_entries.end() ? -1 : it->second.applied;
}

uint32_t ClientViewerTrues(EL::ElementId eid) {
    const auto it = g_viewerCalls.find(eid);
    return it == g_viewerCalls.end() ? 0 : it->second.trues;
}

uint32_t ClientViewerFalses(EL::ElementId eid) {
    const auto it = g_viewerCalls.find(eid);
    return it == g_viewerCalls.end() ? 0 : it->second.falses;
}

}  // namespace coop::creature_event_relay::detail
