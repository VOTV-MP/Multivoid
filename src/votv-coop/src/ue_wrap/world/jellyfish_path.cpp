// ue_wrap/world/jellyfish_path.cpp -- see ue_wrap/world/jellyfish_path.h.

#include "ue_wrap/world/jellyfish_path.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/world_instances.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::jellyfish_path {
namespace {

namespace R = reflection;

// The path's fields. Their places are the class's layout, the same in every world, so each is found once;
// the path itself is looked up per call, a new world bringing a new one.
int32_t g_activeOff = -2;  // isActive; -2 not asked yet, -1 did not resolve
uint8_t g_activeMask = 0;
int32_t g_fishesOff = -2;  // fishes, a TArray<jellyfish_C*>

}  // namespace

void* Path() { return world_singleton::Find(L"jellyfishPath_C"); }

bool CallSpawn() {
    void* path = Path();
    void* fn = path ? R::FindDispatchFunctionCached(R::ClassOf(path), L"spawn") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    return Call(path, f);
}

bool ReadActive(bool& active) {
    void* path = Path();
    if (!path) return false;
    if (g_activeOff == -2) {
        int32_t off = -1;
        g_activeOff = R::FindBoolProperty(R::ClassOf(path), L"isActive", off, g_activeMask) ? off : -1;
        if (g_activeOff < 0) UE_LOGW("jellyfish_path: jellyfishPath_C.isActive did not resolve -- a run cannot be read");
    }
    if (g_activeOff < 0) return false;
    active = (*(reinterpret_cast<const uint8_t*>(path) + g_activeOff) & g_activeMask) != 0;
    return true;
}

bool ReadListed(int& listed) {
    void* path = Path();
    if (!path) return false;
    if (g_fishesOff == -2) {
        g_fishesOff = R::FindPropertyOffset(R::ClassOf(path), L"fishes");
        if (g_fishesOff < 0) UE_LOGW("jellyfish_path: jellyfishPath_C.fishes did not resolve -- its list cannot be read");
    }
    if (g_fishesOff < 0) return false;
    // A TArray is {data, num, max}: the count sits after the data pointer.
    listed = *reinterpret_cast<const int32_t*>(reinterpret_cast<const char*>(path) + g_fishesOff + sizeof(void*));
    return true;
}

int ForEachFish(FishFn fn, void* ctx) {
    void* cls = object_index::ClassByName(L"jellyfish_C");
    if (!cls) return 0;
    struct Walk { FishFn fn; void* ctx; void* world; int n; } walk{fn, ctx, world_identity::CurrentWorld(), 0};
    object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t index) {
        Walk& w = *static_cast<Walk*>(c);
        if (!world_instances::IsTakeable(obj, index, true, w.world)) return;
        FVector at{};
        if (!engine::TryGetActorLocation(obj, at)) return;
        ++w.n;
        if (w.fn) w.fn(w.ctx, obj, at);
    }, &walk);
    return walk.n;
}

}  // namespace ue_wrap::jellyfish_path
