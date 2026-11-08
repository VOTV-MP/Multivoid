// ue_wrap/actors/dig_tool.cpp -- see ue_wrap/actors/dig_tool.h.

#include "ue_wrap/actors/dig_tool.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <string>

namespace ue_wrap::dig_tool {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kToolClasses[] = {L"prop_shovel_C", L"prop_rake_C"};
constexpr const wchar_t* kScrapFn = L"turnIntoScrap";

}  // namespace

bool IsDigTool(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        const std::wstring name = R::ToString(R::NameOf(cls));
        for (const wchar_t* tool : kToolClasses)
            if (name == tool) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

bool TurnIntoScrap(void* pile) {
    if (!pile || !R::IsLive(pile) || !ue_wrap::prop::IsChipPile(pile)) return false;
    // Revalidated on every call, so a world unload cannot hand back a freed function.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pile), kScrapFn);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(pile, f);
}

}  // namespace ue_wrap::dig_tool
