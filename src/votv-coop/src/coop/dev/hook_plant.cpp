// coop/dev/hook_plant.cpp -- see coop/dev/hook_plant.h.

#include "coop/dev/hook_plant.h"

#include "ue_wrap/actors/hook.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

namespace coop::dev::hook_plant {

namespace E  = ue_wrap::engine;
namespace H  = ue_wrap::hook;
namespace PR = ue_wrap::prop;

// The item's own plant, by hand: a hook spawned at the prop, attach_a with the head on the prop's
// mesh and the tail on the player, then the field the item writes so the owner lane adopts it.
//
// The hook is spawned as a MIRROR, not a canonical one: a canonical hook is a save participant,
// and the game's own autosave and save-on-death would write a driver-planted one into the slot,
// where it outlives the run. A mirror is kept out of the save and has its tick disabled, and a
// drag needs neither: the constraint attach_a builds between the prop's mesh and the player's
// root is independent of both.
void* Plant(void* player, void* target, const char** note) {
    void* mesh = PR::GetStaticMesh(target);
    if (!mesh) { *note = "no-static-mesh"; return nullptr; }
    ue_wrap::FVector loc{};
    if (!E::TryGetActorLocation(target, loc)) { *note = "target-location-unread"; return nullptr; }
    void* hook = H::SpawnMirror(H::Kind::Hook, loc, ue_wrap::FRotator{});
    if (!hook) { *note = "hook-did-not-spawn"; return nullptr; }
    if (!H::AttachHead(hook, target, mesh, ue_wrap::FVector{loc.X, loc.Y, loc.Z + 20.f},
                       ue_wrap::FVector{0.f, 0.f, 1.f}, player, /*checkLen=*/true,
                       /*unfreezeFrozen=*/false)) {
        E::DestroyActor(hook);
        *note = "attach_a-did-not-dispatch";
        return nullptr;
    }
    H::WriteActiveHook(player, hook);
    *note = "planted";
    return hook;
}

}  // namespace coop::dev::hook_plant
