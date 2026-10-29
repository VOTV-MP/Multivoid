// ue_wrap/actors/container_box.cpp -- see ue_wrap/actors/container_box.h.

#include "ue_wrap/actors/container_box.h"

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

}  // namespace ue_wrap::container_box
