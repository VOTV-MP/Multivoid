// coop/dev/drill_hand.cpp -- see coop/dev/drill_hand.h.

#include "coop/dev/drill_hand.h"

#include "coop/player/players_registry.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::dev::drill_hand {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

constexpr float kLiftCm = 60.f;   // the item spawns this far above the player's origin

// The hand actor the main hand holds, null when it is empty or the actor is dead.
void* MainHand(void* player) {
    E::MainPlayerGrabState g{};
    if (!E::ReadMainPlayerGrabState(player, g) || !g.mainHand || !R::IsLive(g.mainHand)) return nullptr;
    return g.mainHand;
}

// A no-argument call of the player's own function `name`.
bool CallPlayer(void* player, const wchar_t* name) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(player), name);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(player, f);
}

// The hand emptied the way the game empties a used-up item: the hand actor is destroyed, its destroy binding
// (holdActorDestroy_main) clears the hand's fields and takes the item out of its hotbar slot, which frees the slot. Were
// the fields to stay set, the item is put back into its slot instead.
bool EmptyHand(void* player) {
    void* const hand = MainHand(player);
    if (!hand) return true;
    E::DestroyActor(hand);
    if (MainHand(player) && !CallPlayer(player, L"destroyAndPutBackMainHandItem")) return false;
    return MainHand(player) == nullptr;
}

// The class named by its leaf: loaded already, else the cooked asset at /Game/objects/<leaf>.<leaf>_C.
void* ResolveClass(const wchar_t* cls) {
    if (void* const c = ue_wrap::object_index::ClassByName(cls)) return c;
    std::wstring leaf(cls);
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, L"_C") == 0) leaf.resize(leaf.size() - 2);
    return ue_wrap::asset_load::LoadObjectByPath((L"/Game/objects/" + leaf + L"." + leaf + L"_C").c_str());
}

// `cls` spawned above `at` with its list_props row `item` written before its init pass.
void* SpawnItem(void* cls, const wchar_t* item, const ue_wrap::FVector& at) {
    const ue_wrap::FVector loc{at.X, at.Y, at.Z + kLiftCm};
    const ue_wrap::FRotator rot{};
    void* const actor = E::BeginDeferredSpawn(cls, loc, rot);
    if (!actor) return nullptr;
    if (!ue_wrap::prop::WritePropName(actor, ue_wrap::fname_utils::StringToFName(item)) ||
        !E::FinishDeferredSpawn(actor, loc, rot)) {
        E::DestroyActor(actor);
        return nullptr;
    }
    return actor;
}

// The gas can's amount, written on the finished actor; false when the class has no such property.
bool WriteFuel(void* can, float fuel) {
    static R::InstanceOffset sAmount{L"invRead_amount"};
    const int32_t off = sAmount.Of(can);
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(can) + off, &fuel, sizeof(fuel));
    return true;
}

bool Fail(const wchar_t* item, const char* why) {
    UE_LOGW("drill_hand: '%ls' not taken into the hand -- %s", item, why);
    return false;
}

}  // namespace

bool TakeIntoHandNow(const wchar_t* item, const wchar_t* cls, const float* fuel) {
    void* const player = coop::players::Registry::Get().Local();
    if (!player || !R::IsLive(player)) return Fail(item, "the local player is not live");
    if (!EmptyHand(player)) return Fail(item, "the hand could not be emptied");
    if (!cls) return true;

    void* const klass = ResolveClass(cls);
    ue_wrap::FVector at{};
    if (!klass) return Fail(item, "the class did not load");
    if (!E::TryGetActorLocation(player, at)) return Fail(item, "the player's place is unread");
    void* const actor = SpawnItem(klass, item, at);
    if (!actor) return Fail(item, "the item did not spawn");
    if (fuel && !WriteFuel(actor, *fuel)) {
        E::DestroyActor(actor);
        return Fail(item, "the item has no fuel to write");
    }
    bool taken = false;
    if (!E::CallMainPlayerAddObjectToFreeHotbarSlot(player, actor, E::HotbarAdd{}, taken) || !taken) {
        E::DestroyActor(actor);
        return Fail(item, "the hotbar refused the item");
    }
    void* const hand = MainHand(player);
    if (!hand || R::ClassNameOf(hand) != cls) return Fail(item, "the hand actor is not the item's class");
    return true;
}

}  // namespace coop::dev::drill_hand
