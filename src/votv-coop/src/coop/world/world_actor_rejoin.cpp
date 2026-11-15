// coop/world/world_actor_rejoin.cpp -- see coop/world/world_actor_rejoin.h. The join-time walk of the world-actor lane: every
// class of kWorldActorAllowlist but the ones below, each live instance of this world enrolled once.

#include "coop/world/world_actor_rejoin.h"

#include "coop/element/mirror_manager.h"
#include "coop/element/mirror_managers.h"
#include "coop/element/world_actor.h"
#include "coop/world/world_actor_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"

#include <cstdint>
#include <cwchar>
#include <vector>

namespace coop::world_actor_rejoin {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// The allowlisted classes a joiner is NOT sent by this walk. A class belongs here when the joiner already has the instance
// itself, or when the instance is bound to the one player whose machine runs it, so a second one would be a twin or a haunting.
// Every class not named is host-made at an event and is not in the joiner's world (it is not in a save, nor placed in the level).
constexpr const wchar_t* kNotEnrolled[] = {
    // Level actors: every peer's world holds its own, placed by the level, running its own timers or waiting for its own trigger.
    L"skyUfo_C",                 // the sky UFO has no spawner in the capture; its begin play and two timers are the level's
    L"tentacleBallsFollower_C",  // a level trigger (the eventer calls its runTrigger); its pack is the creature lane's
    // Save actors with no twin adoption (world_actor_mirror adopts only the four named there): a save the joiner loads carries the
    // actor at the pose of the capture, and a copy beside it would be a second saucer or tank. The first connect's snapshot has
    // the same limit.
    L"ufoDropper_body_C", L"ufoDropper_car_C", L"ufoDropper_tank_C", L"ufoDropper_pig_C",  // ufoDropper_C : actor_save_C
    L"firetank_C",               // implements the save interface itself (getData, loadData; ignoresave is false)
    // Bound to one player.
    L"skyFallingEvent_C",        // attaches to player camera manager 0, flashes that player's screen and punches that player
    // A sandbox spawner whose begin play makes its own child on every peer.
    L"piramidSubpawn_C",         // the child of piramidTest_C, not of the piramid event (piramid2_C is enrolled)
};

bool IsNotEnrolled(const wchar_t* cls) {
    for (const wchar_t* n : kNotEnrolled)
        if (std::wcscmp(cls, n) == 0) return true;
    return false;
}

struct Collect {
    void*              world;
    std::vector<void*> found;
};

size_t RowCount() {
    std::vector<coop::element::WorldActor*> rows;
    coop::element::WaMirrors().Snapshot(rows);
    return rows.size();
}

}  // namespace

int EnrolStandingForJoiner(int joinerSlot) {
    void* world = ue_wrap::world_identity::CurrentWorld();
    if (!world) return 0;
    const size_t rowsBefore = RowCount();
    int standing = 0, classes = 0;
    for (size_t i = 0; i < P::name::kWorldActorAllowlistSize; ++i) {
        const wchar_t* name = P::name::kWorldActorAllowlist[i];
        if (IsNotEnrolled(name)) continue;
        void* cls = ue_wrap::object_index::ClassByName(name);
        if (!cls) continue;   // not loaded: no instance
        // Collected first: the index's callback may not touch objects, and the enrol reads each actor's pose through the engine.
        Collect c{world, {}};
        ue_wrap::object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
            auto* col = static_cast<Collect*>(ctx);
            if (obj) col->found.push_back(obj);
        }, &c);
        int live = 0;
        const size_t rowsAtClass = RowCount();
        for (void* obj : c.found) {
            if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;
            if (ue_wrap::world_identity::WorldOf(obj) != world) continue;   // a world since unloaded
            ++live;
            coop::world_actor_sync::HostEnrollExSpawn(obj);   // an actor the lane tracks already answers with its row
        }
        if (live == 0) continue;
        ++classes;
        standing += live;
        // The table's reading of the capture, checked on the live class: a class that carries actor_save_C's `skipSave` is a save
        // actor, and one the mirror does not adopt a twin of is a class the joiner's own load may hold a second one of.
        int32_t off = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(cls, L"skipSave", off, mask) && !coop::world_actor_sync::AdoptsSaveTwin(name))
            UE_LOGW("world-actor[join-enrol]: '%ls' is a save actor (it has skipSave) the mirror adopts no twin of: a save the "
                    "joiner loaded may hold its own beside the one sent", name);
        UE_LOGI("world-actor[join-enrol]: slot %d: %d live '%ls' (%zu enrolled now)", joinerSlot, live, name,
                RowCount() - rowsAtClass);
    }
    const int enrolled = static_cast<int>(RowCount() - rowsBefore);
    if (standing > 0)
        UE_LOGI("world-actor[join-enrol]: slot %d: %d live actor(s) of %d class(es) stand, %d enrolled now (the rest the lane "
                "already held)", joinerSlot, standing, classes, enrolled);
    return enrolled;
}

}  // namespace coop::world_actor_rejoin
