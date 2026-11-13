// coop/dev/prop_act_drill_mre.cpp -- the act drill's MRE step, client side: the reads its contents and poison waits are made
// of. See coop/dev/prop_act_drill.h.

#include "coop/dev/prop_act_drill.h"

#include "coop/element/registry.h"
#include "coop/props/prop_food_sync.h"

#include "ue_wrap/actors/effects.h"
#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <cwchar>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::dev::prop_act_drill::detail {

namespace EL = coop::element;
namespace R  = ue_wrap::reflection;

// The prop_food_C mirrors this client holds. `*all` collects their ids; the count is those `had` lacks, and `*poisoned`
// names the first of them whose poison fields read above 0 (what the host's FoodState carried onto it). `*waiting` is set
// when a FoodState for one of those fresh mirrors still waits for its copy (prop_food_sync's pending set): what the copy
// shows is not final until it has drained.
size_t FoodMirrorsNotIn(const std::unordered_set<EL::ElementId>& had, std::unordered_set<EL::ElementId>* all,
                        void** poisoned, bool* waiting) {
    static std::vector<EL::Registry::ActorIdPair> pairs;
    EL::Registry::Get().SnapshotActorsByType(EL::ElementType::Prop, pairs);
    size_t fresh = 0;
    for (const auto& p : pairs) {
        if (!p.mirror || !p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) || !ue_wrap::food::IsFood(p.actor))
            continue;
        if (all) all->insert(p.id);
        if (had.count(p.id)) continue;
        ++fresh;
        if (waiting && coop::props::prop_food_sync::dev::StateWaiting(ue_wrap::prop::GetInteractableKeyString(p.actor)))
            *waiting = true;
        ue_wrap::food::Poison poison;
        if (poisoned && !*poisoned && ue_wrap::food::ReadPoison(p.actor, poison) && poison.delay > 0.f &&
            poison.strength > 0.f)
            *poisoned = p.actor;
    }
    return fresh;
}

int FoodPoisonLive(const std::vector<ue_wrap::effects::Entry>& list) {
    int n = 0;
    for (const auto& e : list)
        if (e.live && _wcsicmp(e.name.c_str(), L"foodPoison") == 0) ++n;
    return n;
}

// The food's `bitten(player)`: the bite's own poison branch (prop_food.cpp), which a press on the food reaches.
bool CallBitten(void* food, void* player) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(food), L"bitten");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && ue_wrap::Call(food, f);
}

}  // namespace coop::dev::prop_act_drill::detail
