// coop/dev/drive_drill_verbs.cpp -- see drive_drill_verbs.h.

#include "drive_drill_verbs.h"  // co-located private header (src tree, not include/)

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <string>

namespace coop::dev::drive_drill_verbs {
namespace {

namespace R = ue_wrap::reflection;

void* PlayerFn(void* player, const wchar_t* name) {
    return player ? R::FindDispatchFunctionCached(R::ClassOf(player), name) : nullptr;
}

}  // namespace

bool Pocket(void* player, void* prop) {
    bool taken = false;
    return ue_wrap::engine::CallPutObjectInventory2(player, prop, true, taken) && taken;
}

void* TakeOut(const std::wstring& cls, const std::wstring& key) {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return nullptr;
    int32_t index = -1;
    for (size_t i = 0; i < store.records.size(); ++i)
        if (store.records[i].className == cls && store.records[i].key == key) index = static_cast<int32_t>(i);
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"playerContainer") : -1;
    void* container = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + off) : nullptr;
    if (index < 0 || !container || !R::IsLive(container)) return nullptr;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(container), L"getObject");
    ue_wrap::ParamFrame f(fn);
    const ue_wrap::FVector none{};
    if (!fn || !f.valid() || !f.Set<int32_t>(L"index", index) || !f.Set(L"customLoc", false) ||
        !f.SetRaw(L"loc", &none, sizeof(none)) || !ue_wrap::Call(container, f))
        return nullptr;
    void* out = f.Get<void*>(L"OutputPin");
    return out && R::IsLive(out) ? out : nullptr;
}

bool Hold(void* player, void* prop) {
    bool collected = false;
    return ue_wrap::engine::CallMainPlayerHoldObject(player, prop, collected) && collected;
}

bool Throw(void* player) {
    void* fn = PlayerFn(player, L"throwHoldingProp");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && ue_wrap::Call(player, f);
}

}  // namespace coop::dev::drive_drill_verbs
