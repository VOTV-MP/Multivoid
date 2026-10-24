// ue_wrap/actors/sleepingbag.cpp -- see ue_wrap/actors/sleepingbag.h.

#include "ue_wrap/actors/sleepingbag.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::sleepingbag {
namespace {

namespace R = ue_wrap::reflection;

void* FindClass(const wchar_t* name) {
    return R::FindClass(name);
}

bool IsChild(void* actor, const wchar_t* baseName) {
    if (!actor || !R::IsLive(actor)) return false;
    void* base = FindClass(baseName);
    void* cls = R::ClassOf(actor);
    if (!base || !cls) return false;
    void* bases[] = {base};
    return R::IsDescendantOfAny(cls, bases, 1);
}

}  // namespace

bool IsPacked(void* actor) {
    return IsChild(actor, L"prop_sleepingbag_C");
}

bool IsDeployed(void* actor) {
    return IsChild(actor, L"sleepingbagWrap_C");
}

bool CallUnwrap(void* actor, void* player, uint8_t action) {
    if (!IsPacked(actor)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame frame(fn);
    if (!frame.valid() || !frame.Set<void*>(L"Player", player) || !frame.Set<uint8_t>(L"Action", action))
        return false;
    return Call(actor, frame);
}

bool CallFold(void* actor) {
    if (!IsDeployed(actor)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(actor), L"fold");
    if (!fn) return false;
    ParamFrame frame(fn);
    return frame.valid() && Call(actor, frame);
}

}  // namespace ue_wrap::sleepingbag
