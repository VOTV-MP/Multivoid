// coop/creatures/hit_target_creature.cpp -- see coop/creatures/hit_target_creature.h.

#include "coop/creatures/hit_target_creature.h"

#include "coop/creatures/npc_mirror_brain.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/hit_intent.h"

#include "ue_wrap/actors/int_objects.h"
#include "ue_wrap/actors/zombie.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cstdint>
#include <cwchar>

namespace coop::hit_target_creature {
namespace {

namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
namespace P  = ue_wrap::profile;
namespace sg = ue_wrap::script_gate;
using ue_wrap::FVector;

// The functions the zombie family's prediction played, said once each per session.
constexpr int kMaxPredicted = 4;
const wchar_t* g_predicted[kMaxPredicted] = {};
int            g_predictedCount = 0;

void SayPredicted(const wchar_t* function) {
    for (int i = 0; i < g_predictedCount; ++i)
        if (std::wcscmp(g_predicted[i], function) == 0) return;
    if (g_predictedCount < kMaxPredicted) g_predicted[g_predictedCount++] = function;
    UE_LOGI("creature_hit_intent: predicted %ls", function);
}

// The cosmetic half of a hit: the family's feedback at once, so the swing does not wait on the host. Only the zombie
// family has one; the fossilhound's single hit sound sits in a refused resume.
// Source plays a melee swing's impact effect on both sides and its damage on the server alone
// (reference/source-sdk-2013/src/game/shared/hl2mp/weapon_hl2mpbasebasebludgeon.cpp:149-172), and withholds the
// server's copy of the shooter's own sounds from the shooter (reference/source-sdk-2013/src/game/shared/
// basecombatweapon_shared.cpp:1925-1930). This takes the first half: the swinger's feedback is local, health and
// death are the host's. The second half, delivering the server's copy to every peer but the swinger, is not taken: the
// host's own call plays its own feedback, a third client sees none, and a swing the host refuses has already shown its
// feedback.
void Predict(void* mirror, const FVector& hitLoc) {
    void* zombie = ue_wrap::object_index::ClassByName(P::name::NpcClass_Zombie);
    if (!zombie || !R::IsDescendantOfAny(R::ClassOf(mirror), &zombie, 1)) return;
    if (const wchar_t* played = ue_wrap::zombie::PlayHitFeedback(mirror, hitLoc)) SayPredicted(played);
}

// The creature the intent names: a live host Npc element of a parked family, else null.
void* CreatureOf(uint32_t eid) {
    void* actor = EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Npc);
    return actor && coop::npc_mirror_brain::IsParkedClass(R::ClassOf(actor)) ? actor : nullptr;
}

}  // namespace

uint32_t ClientClaim(const sg::Call& c) {
    if (!c.object || !coop::npc_mirror_brain::IsMarkedMirror(c.object) ||
        !coop::npc_mirror_brain::IsParkedClass(R::ClassOf(c.object)))
        return EL::kInvalidId;

    coop::hit_intent::Swing swing;
    if (!coop::hit_intent::ReadLocalSwing(c, swing)) return EL::kInvalidId;

    const EL::ElementId eid = EL::Registry::Get().EidForActor(c.object);
    if (eid == EL::kInvalidId) return EL::kInvalidId;

    Predict(c.object, swing.hitLoc);
    return static_cast<uint32_t>(eid);
}

void* HostTarget(uint32_t eid) { return CreatureOf(eid); }

bool HostApply(uint32_t eid, const coop::net::CreatureHitIntentPayload& judged) {
    void* creature = CreatureOf(eid);
    if (!creature) return false;
    const FVector hitLoc{judged.hitLoc[0], judged.hitLoc[1], judged.hitLoc[2]};
    const FVector impact{judged.impact[0], judged.impact[1], judged.impact[2]};
    return ue_wrap::int_objects::InvokeAddDamage(creature, nullptr, judged.damage, hitLoc, impact);
}

void OnDisconnect() { g_predictedCount = 0; }

}  // namespace coop::hit_target_creature
