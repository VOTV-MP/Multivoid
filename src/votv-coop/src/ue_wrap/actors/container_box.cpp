// ue_wrap/actors/container_box.cpp -- see ue_wrap/actors/container_box.h.

#include "ue_wrap/actors/container_box.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::container_box {
namespace {

namespace R = reflection;

// The component pointer a container's property `off` holds, live, or null.
void* ComponentAt(void* container, int32_t off) {
    if (off < 0) return nullptr;
    void* comp = nullptr;
    std::memcpy(&comp, static_cast<const uint8_t*>(container) + off, sizeof(comp));
    return comp && R::IsLive(comp) ? comp : nullptr;
}

}  // namespace

void* OverlapComponent(void* container) {
    static R::InstanceOffset sOverlap{L"overlap"};
    if (!container || !R::IsLive(container)) return nullptr;
    return ComponentAt(container, sOverlap.Of(container));
}

bool IsRoomba(void* actor) {
    void* const roomba = ue_wrap::object_index::ClassByName(L"prop_roomba_C");
    return actor && roomba && R::IsLive(actor) && R::IsDescendantOfAny(R::ClassOf(actor), &roomba, 1);
}

bool OverlapsActor(void* container, void* actor) {
    void* const box = OverlapComponent(container);
    if (!box || !actor || !R::IsLive(actor)) return false;
    // IsOverlappingActor is UPrimitiveComponent's; the dispatch cache climbs to it.
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(box), L"IsOverlappingActor");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"Other", actor);
    return Call(box, f) && f.Get<bool>(L"ReturnValue");
}

bool CallPutObjectInOverlap(void* container, void* actor, void* component) {
    static R::InstanceOffset sAudio{L"Audio"};
    if (!container || !R::IsLive(container) || !actor || !component) return false;
    void* const audio = ComponentAt(container, sAudio.Of(container));
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(container), L"putObjectIn_overlap");
    if (!audio || !fn) return false;
    ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"actor", actor) && f.Set<void*>(L"component", component) &&
           f.Set<void*>(L"audio", audio) && Call(container, f);
}

}  // namespace ue_wrap::container_box
