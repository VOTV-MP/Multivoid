// coop/creatures/creature_hit.cpp -- see coop/creatures/creature_hit.h.

#include "coop/creatures/creature_hit.h"

#include "coop/creatures/npc_mirror_brain.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/reflection.h"

namespace coop::creature_hit {

namespace R  = ue_wrap::reflection;
namespace EL = coop::element;

bool IsCreature(void* actor) {
    if (!actor) return false;
    const EL::Registry& reg = EL::Registry::Get();
    const EL::ElementId eid = reg.EidForActor(actor);
    if (eid == EL::kInvalidId) return false;
    const EL::Element* el = reg.Get(eid);
    return el && el->GetType() == EL::ElementType::Npc;
}

namespace {

// A Rozital's shot (tentacleBallShot_C, and kavotiaShot_C under it): its shooters, the scouts' balls and the Hole's
// guards, are mirrors on a client whose charge and tick are refused (coop/world/spawn_authority), so a client fires
// none and the host's shot is the only one.
bool IsRozitalShot(void* actor) {
    int hops = 0;
    for (void* cls = actor ? R::ClassOf(actor) : nullptr; cls && hops < 4; cls = R::SuperStructOf(cls), ++hops)
        if (R::ToString(R::NameOf(cls)) == L"tentacleBallShot_C") return true;
    return false;
}

}  // namespace

bool IsDelivered(void* actor) {
    if (ue_wrap::ambient_creature::IsWalker(actor)) return true;  // a prop, not an Npc, and its copy's timers are off
    if (IsRozitalShot(actor)) return true;
    return IsCreature(actor) && coop::npc_mirror_brain::IsParkedClass(R::ClassOf(actor));
}

}  // namespace coop::creature_hit
