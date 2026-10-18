// ue_wrap/engine/engine_player_verbs.cpp -- see ue_wrap/engine/engine_player_verbs.h.

#include "ue_wrap/engine/engine_player_verbs.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::engine {
namespace {

namespace R = reflection;

// The verb as `player`'s class dispatches it, memoised per class by the reflection layer.
void* VerbOf(void* player, const wchar_t* name) {
    if (!player || !R::IsLive(player)) return nullptr;
    void* cls = R::ClassOf(player);
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

}  // namespace

bool InvokeAddDamage(void* player, void* actor, float damage, const FVector& impact) {
    void* fn = VerbOf(player, L"addDamage");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"actor", actor);
    f.Set<float>(L"damage", damage);
    f.Set<FVector>(L"impact", impact);
    return Call(player, f);
}

bool InvokePunch(void* player, const FVector& fullBody, const FVector& boneImp, const wchar_t* boneName,
                 float damage, const FVector& damageLocation) {
    void* fn = VerbOf(player, L"punch");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    const R::FName bone = boneName ? fname_utils::StringToFName(boneName) : R::FName{};
    f.Set<FVector>(L"fullBody", fullBody);
    f.Set<FVector>(L"boneImp", boneImp);
    f.SetRaw(L"bn1", &bone, sizeof(bone));
    f.Set<float>(L"damage", damage);
    f.Set<FVector>(L"damageLocation", damageLocation);
    return Call(player, f);
}

}  // namespace ue_wrap::engine
