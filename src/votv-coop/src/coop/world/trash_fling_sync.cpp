// coop/world/trash_fling_sync.cpp -- see coop/world/trash_fling_sync.h.
//
// The bytecode this stands on (arirTrasher_C.trash, bound to a 1 s looping timer at BeginPlay): when the host's
// player pawn is within 5000 uu of the box, up to ten times it asks the box for its overlapping prop_C actors, picks
// one at random, and if its root simulates physics adds a velocity of (250..1000, 0, 0) turned by a random yaw and an
// angular velocity of up to 500 deg/s (SetPhysicsLinearVelocity with bAddToCurrent). It then counts itself down
// (at zero it ends its event and destroys itself), and begin-spawns a prop_garbageClump_C 25 uu above the prop, sets
// its chipType, finishes it and gives its mesh a velocity of 1000 uu/s in a random direction.
//
// The watch is a post watch, after the body has run: the flung props are the simulating props inside the box that
// move fast now, found in the next tick from the registry (no engine array is allocated or freed here), and the
// trasher's reach is read inside the callback, since the last call of its count destroys it.

#include "coop/world/trash_fling_sync.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/trash_sweep.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::trash_fling_sync {
namespace {

namespace SG = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace PT = coop::prop_element_tracker;

UE_FACT_FN(kRowTrash, "trash_fling", L"arirTrasher_C", L"trash", Declared);
constexpr int kTagTrash = 0x54524648;  // 'TRFH'

// A prop the prank adds 2.5 m/s or more to is fast for a moment after, whatever it was doing before; below this a
// prop inside the box is resting or rolling on its own.
constexpr float kFlungMinCmS = 150.f;
// The reach read from the trasher's bounds is its colliding box; a prop is inside it a frame after the fling, and
// the clump spawns 25 uu above the prop it was flung beside.
constexpr float kReachMarginUU = 100.f;
constexpr size_t kMaxQueued = 16;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_watchTaken = false;
bool g_namesSettled = false;

// One `trash` call: where the trasher's box reaches. Read in the callback, used in the next tick.
struct Fired {
    ue_wrap::FVector centre;
    float            radius;
};
std::vector<Fired> g_fired;
std::vector<coop::element::Registry::ActorIdPair> g_props;  // reused by every Tick

struct Clump {
    void*   actor;
    int32_t index;
};
std::vector<Clump> g_clumps;  // reused by every Tick

uint64_t g_flungTotal = 0;
uint64_t g_clumpsTotal = 0;

void OnTrashPost(const SG::Call& c) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!c.object || !s || s->role() != coop::net::Role::Host || g_fired.size() >= kMaxQueued) return;
    Fired f{};
    if (!E::ActorReachSphere(c.object, f.centre, f.radius) || f.radius <= 0.f) {
        static bool sSaid = false;
        if (!sSaid) {
            sSaid = true;
            UE_LOGW("[TRASH-FLING] the trasher's box reach could not be read -- its flung props are not coasted");
        }
        return;
    }
    f.radius += kReachMarginUU;
    g_fired.push_back(f);
}

bool Inside(const Fired& f, const ue_wrap::FVector& p) {
    const float dx = p.X - f.centre.X, dy = p.Y - f.centre.Y, dz = p.Z - f.centre.Z;
    return dx * dx + dy * dy + dz * dz <= f.radius * f.radius;
}

void CollectClump(void* /*ctx*/, void* obj, int32_t index) { g_clumps.push_back(Clump{obj, index}); }

// A clump the game made with no pile: its element id, the spawn row to every ready peer (with its carry
// generation behind it), and the roll on the trash carry lane until it lands.
bool Announce(void* clump) {
    PT::MarkPropElement(clump, L"", R::ClassNameOf(clump), PT::EnrollSource::kPassiveCensus);
    const coop::element::ElementId eid = PT::GetPropElementIdForActor(clump);
    if (eid == coop::element::kInvalidId || eid == 0u) return false;
    coop::prop_snapshot::DeliverLateRegisteredProps(std::vector<void*>{clump});
    coop::trash_sweep::NotePushed(clump);
    return true;
}

}  // namespace

void Install(coop::net::Session* s) {
    g_session.store(s, std::memory_order_release);
    if (g_watchTaken) return;
    g_watchTaken = SG::WatchName(kRowTrash, kTagTrash, nullptr, &OnTrashPost);
    static bool sSaid = false;
    if (!g_watchTaken && !sSaid)
        UE_LOGE("[TRASH-FLING] the watch on arirTrasher_C::trash was refused -- retried each pump");
    sSaid = !g_watchTaken;
}

void Tick(coop::net::Session* s) {
    if (g_watchTaken && !g_namesSettled) {
        SG::ResolvePendingNames();
        g_namesSettled = SG::NameWatchSettled(kRowTrash, kTagTrash);
        if (g_namesSettled && !SG::NameWatchLive(kRowTrash, kTagTrash))
            UE_LOGE("[TRASH-FLING] the watch on arirTrasher_C::trash is dead -- the prank's props reach no client");
        else if (g_namesSettled)
            UE_LOGI("[TRASH-FLING] the watch on arirTrasher_C::trash is live");
    }
    if (g_fired.empty()) return;
    if (!s || !s->connected() || s->role() != coop::net::Role::Host || s->connectedPeerCount() <= 0) {
        g_fired.clear();
        return;
    }

    // One registry pass for every call of this tick. A flung prop is a simulating Aprop_C inside a call's reach with
    // a speed past the floor; the drive streams it to rest. Anything else the registry holds there is left alone.
    unsigned flung = 0;
    char     eids[96] = {};
    size_t   used = 0;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_props);
    for (const auto& prop : g_props) {
        if (!prop.actor || !R::IsLiveByIndex(prop.actor, prop.internalIdx)) continue;
        ue_wrap::FVector pos{};
        if (!E::TryGetRootLocationRaw(prop.actor, pos) && !E::TryGetActorLocation(prop.actor, pos)) continue;
        bool inside = false;
        for (const Fired& f : g_fired) inside = inside || Inside(f, pos);
        if (!inside || !PR::IsDescendantOfProp(prop.actor)) continue;
        const PR::VelocityState v = PR::GetPhysicsVelocity(prop.actor, /*linearOnly=*/true);
        const float l2 = v.linearCmS.X * v.linearCmS.X + v.linearCmS.Y * v.linearCmS.Y + v.linearCmS.Z * v.linearCmS.Z;
        if (!v.ok || l2 < kFlungMinCmS * kFlungMinCmS) continue;
        coop::prop_drive_host::Coast(prop.actor, "trash fling");
        if (flung < 8 && used < sizeof(eids))
            used += static_cast<size_t>(std::snprintf(eids + used, sizeof(eids) - used, "%s%u", flung ? "," : "",
                                                      static_cast<unsigned>(prop.id)));
        ++flung;
    }

    // The clumps the call spawned: instances of the class with no element id yet, inside a reach. One that already
    // has an id is a pile's clump the trash channel owns.
    unsigned announced = 0;
    g_clumps.clear();
    if (void* cls = ue_wrap::object_index::ClassByName(L"prop_garbageClump_C"))
        ue_wrap::object_index::ForEachInstance(cls, &CollectClump, nullptr);
    for (const Clump& cl : g_clumps) {
        if (!R::IsLiveByIndex(cl.actor, cl.index)) continue;
        const coop::element::ElementId have = PT::GetPropElementIdForActor(cl.actor);
        if (have != coop::element::kInvalidId && have != 0u) continue;
        ue_wrap::FVector pos{};
        if (!E::TryGetRootLocationRaw(cl.actor, pos) && !E::TryGetActorLocation(cl.actor, pos)) continue;
        bool inside = false;
        for (const Fired& f : g_fired) inside = inside || Inside(f, pos);
        if (inside && Announce(cl.actor)) ++announced;
    }
    g_fired.clear();

    g_flungTotal += flung;
    g_clumpsTotal += announced;
    if (flung || announced)
        UE_LOGI("[TRASH-FLING] coast n=%u eids=%s; %u clump(s) announced (session %llu coasted, %llu clumps)", flung,
                eids, announced, static_cast<unsigned long long>(g_flungTotal),
                static_cast<unsigned long long>(g_clumpsTotal));
}

void OnDisconnect() {
    g_fired.clear();
    g_clumps.clear();
    if (g_flungTotal || g_clumpsTotal)
        UE_LOGI("[TRASH-FLING] session end: %llu flung prop(s) coasted, %llu clump(s) announced",
                static_cast<unsigned long long>(g_flungTotal), static_cast<unsigned long long>(g_clumpsTotal));
    g_flungTotal = g_clumpsTotal = 0;
}

}  // namespace coop::trash_fling_sync
