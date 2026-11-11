// coop/dev/flashlight_setup.cpp -- autotest-only flashlight setup helpers. See
// coop/dev/flashlight_setup.h.

#include "coop/dev/flashlight_setup.h"

#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <cstdint>

namespace coop::dev::flashlight_setup {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Cached reflection state (resolved on first call, then constant).
struct Cache {
    void* flashlightClass = nullptr;    // UClass* for prop_equipment_flashlight_C
    R::FName flashlightClassName{0, 0}; // FName of that UClass (= "prop_equipment_flashlight_C")
    void* mainPlayerClass = nullptr;
    void* addPropToPlayerFn = nullptr;
};
Cache g_cache;

bool EnsureReflectionResolved(void* mainPlayer) {
    if (!mainPlayer) return false;

    if (!g_cache.flashlightClass) {
        g_cache.flashlightClass = R::FindClass(P::name::FlashlightEquipmentClass);
        if (!g_cache.flashlightClass) {
            UE_LOGW("flashlight_setup: prop_equipment_flashlight_C class not yet alive");
            return false;
        }
        // The UClass UObject's NamePrivate IS the class's FName -- this is
        // exactly what AmainPlayer_C::addPropToPlayer(FName) expects.
        g_cache.flashlightClassName = R::NameOf(g_cache.flashlightClass);
    }
    if (!g_cache.mainPlayerClass) {
        g_cache.mainPlayerClass = R::ClassOf(mainPlayer);
        if (!g_cache.mainPlayerClass) return false;
    }
    if (!g_cache.addPropToPlayerFn) {
        g_cache.addPropToPlayerFn =
            R::FindFunction(g_cache.mainPlayerClass, P::name::MainPlayerAddPropToPlayerFn);
        if (!g_cache.addPropToPlayerFn) {
            UE_LOGW("flashlight_setup: addPropToPlayer UFunction not found on mainPlayer_C");
            return false;
        }
    }
    return true;
}

}  // namespace

bool GiveFlashlight(void* mainPlayer) {
    if (!EnsureReflectionResolved(mainPlayer)) return false;
    // addPropToPlayer(FName prop). The FName the UFunction expects is
    // exactly the UClass's name (e.g. "prop_equipment_flashlight_C").
    // The live UClass UObject's NamePrivate IS that FName -- no string
    // construction needed; we cached it in EnsureReflectionResolved.
    ue_wrap::ParamFrame f(g_cache.addPropToPlayerFn);
    f.SetRaw(L"prop", &g_cache.flashlightClassName, sizeof(g_cache.flashlightClassName));
    const bool ok = ue_wrap::Call(mainPlayer, f);
    UE_LOGI("flashlight_setup: GiveFlashlight: addPropToPlayer(prop=%ls / FName{idx=%u,num=%u}) -> %d",
            P::name::FlashlightEquipmentClass,
            g_cache.flashlightClassName.ComparisonIndex,
            g_cache.flashlightClassName.Number, ok ? 1 : 0);
    return ok;
}

bool SetBatteryFull(void* mainPlayer) {
    if (!EnsureReflectionResolved(mainPlayer)) return false;
    // VOTV's battery scale is 0-100 (a percentage), NOT 0-1. The charge is the worn flashlight's own `battery`,
    // inside its inventory entry, which ticker_playerFlashlight drains.
    const bool ok = ue_wrap::inventory::WriteWornItemProperty(P::name::FlashlightEquipmentClass, L"battery", L"100");
    UE_LOGI("flashlight_setup: SetBatteryFull: the worn flashlight's battery -> 100 (%s)",
            ok ? "written" : "NOT written: no flashlight worn, or the write failed");
    return ok;
}

bool EnsureFlashlightReady(void* mainPlayer) {
    if (!EnsureReflectionResolved(mainPlayer)) return false;

    ue_wrap::engine::MainPlayerFlashlightState fl{};
    ue_wrap::engine::ReadMainPlayerFlashlightState(mainPlayer, fl);
    const bool hadFlashlight = fl.equippedFlashlight;
    UE_LOGI("flashlight_setup: pre-state: equipped_flashlight=%d", hadFlashlight ? 1 : 0);

    // Give the flashlight if missing. addPropToPlayer may stack duplicates, so it is skipped while
    // equipped_flashlight is already true.
    if (!hadFlashlight) {
        GiveFlashlight(mainPlayer);
    } else {
        UE_LOGI("flashlight_setup: equipped_flashlight already true -- skipping GiveFlashlight");
    }

    // Always top off the battery -- the save may have a discharged one.
    const bool charged = SetBatteryFull(mainPlayer);

    ue_wrap::engine::ReadMainPlayerFlashlightState(mainPlayer, fl);
    const bool nowHasFlashlight = fl.equippedFlashlight;
    UE_LOGI("flashlight_setup: post-state: equipped_flashlight=%d charged=%d -- ready=%d",
            nowHasFlashlight ? 1 : 0, charged ? 1 : 0, (nowHasFlashlight && charged) ? 1 : 0);
    return nowHasFlashlight && charged;
}

}  // namespace coop::dev::flashlight_setup
