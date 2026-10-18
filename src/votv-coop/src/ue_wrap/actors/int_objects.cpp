// ue_wrap/actors/int_objects.cpp -- see ue_wrap/actors/int_objects.h.

#include "ue_wrap/actors/int_objects.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/obj_keyed_cache.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/hit_result.h"

#include <cstdint>

namespace ue_wrap::int_objects {
namespace {

namespace R = reflection;

// The actor's ACharacter Mesh component, found by its reflected name; null when the class has none or the slot
// is empty. The offset is resolved once per class (-1 kept for a class with none): the family's classes alternate.
void* MeshOf(void* actor) {
    static ObjKeyedCache<int32_t, 8> sMeshOffsets;
    void* cls = R::ClassOf(actor);
    if (!cls) return nullptr;
    const int32_t* held = sMeshOffsets.Find(cls);
    const int32_t off = held ? *held : sMeshOffsets.Insert(cls, R::FindPropertyOffset(cls, L"Mesh"));
    if (off < 0) return nullptr;
    void* mesh = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(actor) + off);
    return (mesh && R::IsLive(mesh)) ? mesh : nullptr;
}

}  // namespace

bool InvokeAddDamage(void* target, void* actor, float damage, const FVector& hitLoc, const FVector& impact) {
    if (!target || !R::IsLive(target)) return false;
    void* cls = R::ClassOf(target);
    void* fn = cls ? R::FindDispatchFunctionCached(cls, L"addDamage") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"actor", actor);
    f.Set<float>(L"damage", damage);
    f.Set<FVector>(L"impact", impact);
    if (!hit_result::Write(f, L"hit", target, MeshOf(target), hitLoc)) return false;
    return Call(target, f);
}

}  // namespace ue_wrap::int_objects
