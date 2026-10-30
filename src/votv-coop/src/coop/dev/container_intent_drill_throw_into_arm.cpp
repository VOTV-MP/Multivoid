// coop/dev/container_intent_drill_throw_into_arm.cpp -- the throwinto arm arms its target's trigger the game's own way. A
// container's `overlap` box is NoCollision from spawn; only the container's own dingus() turns it on, through a retriggerable delay
// of overlapDelay (0.1 s) and then SetCollisionEnabled(QueryOnly), and the game calls it after an extract, a get or a put. A
// fresh basket therefore ignores a thrown prop. The arm calls dingus() on the HOST's actor only (the client's mirror takes its box from the
// host's flag, container_arming) and reads the box's collision-enabled state before and after, by reflection.

#include "container_intent_drill_throw_into.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>

namespace coop::dev::container_intent_drill::detail::into {
namespace R = ue_wrap::reflection;

void* TriggerComponent(void* container) {
    if (!container) return nullptr;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(container), L"overlap");
    void* comp = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(container) + off) : nullptr;
    return comp && R::IsLive(comp) ? comp : nullptr;
}

int CollisionOf(void* comp) {
    void* fn = comp ? R::FindDispatchFunctionCached(R::ClassOf(comp), L"GetCollisionEnabled") : nullptr;
    ue_wrap::ParamFrame frame(fn);
    if (!fn || !frame.valid() || !ue_wrap::Call(comp, frame)) return -1;
    return frame.Get<uint8_t>(L"ReturnValue");
}

int TriggerState(void* container) { return CollisionOf(TriggerComponent(container)); }

bool CallParamless(void* obj, const wchar_t* fnName) {
    void* fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), fnName) : nullptr;
    ue_wrap::ParamFrame frame(fn);
    return fn && frame.valid() && ue_wrap::Call(obj, frame);
}

bool CallDingus(void* container) { return CallParamless(container, L"dingus"); }

bool UnarmedOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::container_throwinto_unarmed);
    return on;
}

}  // namespace coop::dev::container_intent_drill::detail::into
