// ue_wrap/actors/item_spawn.cpp -- see ue_wrap/actors/item_spawn.h.

#include "ue_wrap/actors/item_spawn.h"

#include "ue_wrap/core/class_resolve.h"

namespace ue_wrap::item_spawn {

void* LoadedClass(const wchar_t* className) {
    // Immediate, outside the per-tick load budget: the caller returns its item in place and has nothing to park, as the
    // hook mirror's variant class is built the moment it is asked for.
    const class_resolve::Resolved r =
        class_resolve::ResolveClass(className, class_resolve::AnyFamily(), class_resolve::Load::Immediate);
    return r.state == class_resolve::ClassState::Found && r.cls ? r.cls : nullptr;
}

void* Spawn(const wchar_t* className, const FVector& at, engine::BeforeFinishFn beforeFinish, void* ctx) {
    void* const cls = LoadedClass(className);
    return cls ? engine::SpawnActor(cls, at, false, beforeFinish, ctx) : nullptr;
}

}  // namespace ue_wrap::item_spawn
