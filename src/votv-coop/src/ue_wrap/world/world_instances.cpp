// ue_wrap/world/world_instances.cpp -- see ue_wrap/world/world_instances.h.
#include "ue_wrap/world/world_instances.h"

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/world_identity.h"

namespace ue_wrap::world_instances {
namespace {

namespace R = reflection;
namespace P = profile;

constexpr int32_t kRF_ClassDefaultObject = 0x10;  // EObjectFlags (UE4.27), at UObject_ObjectFlags

}  // namespace

bool IsTakeable(void* obj, int32_t index, bool actorClass, void* runningWorld) {
    if (!obj) return false;
    if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return false;
    if (*reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + P::off::UObject_ObjectFlags) &
        kRF_ClassDefaultObject)
        return false;
    void* const w = world_identity::WorldOf(obj);
    if (actorClass && !w) return false;
    return !(w && runningWorld && w != runningWorld);
}

bool IsActorClass(void* cls) {
    static void* s_actor = nullptr;  // a native class: loaded once, never re-created
    if (!s_actor) s_actor = object_index::ClassByName(P::name::ActorClass);
    return cls && s_actor && R::IsDescendantOfAny(cls, &s_actor, 1);
}

int32_t Find(const wchar_t* className, void** out, int32_t cap) {
    UE_ASSERT_GAME_THREAD("world_instances::Find");
    if (!className || !out || cap <= 0) return 0;
    void* cls = object_index::ClassByName(className);
    if (!cls) return 0;
    struct Fill {
        void**  out;
        int32_t cap, n;
        bool    actor;
        void*   world;
    } f{out, cap, 0, IsActorClass(cls), world_identity::CurrentWorld()};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        Fill& f = *static_cast<Fill*>(ctx);
        if (f.n < f.cap && IsTakeable(obj, index, f.actor, f.world)) f.out[f.n++] = obj;
    }, &f);
    return f.n;
}

}  // namespace ue_wrap::world_instances
