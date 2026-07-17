// coop/dev/flashlight_setup.cpp -- autotest-only flashlight setup helpers.
// See coop/dev/flashlight_setup.h and
// research/findings/inventory-items/votv-inventory-equip-battery-RE-2026-05-26.md.

#include "coop/dev/flashlight_setup.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/types.h"

#include <cstdint>

namespace coop::dev::flashlight_setup {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// Cached reflection state (resolved on first call, then constant).
struct Cache {
    void* mainGameInstance = nullptr;   // UmainGameInstance_C*
    int32_t saveGameInstOff = -1;       // mainGameInstance_C::save_gameInst
    void* saveSlotClass = nullptr;
    int32_t batteryOff = -1;            // saveSlot.battery (float)
    int32_t flashlightBatteryOff = -1;  // saveSlot.flashlightBattery (TSubclassOf<Aprop_batts_C>)
    void* battsClass = nullptr;         // UClass* for prop_batts_C
    void* flashlightClass = nullptr;    // UClass* for prop_equipment_flashlight_C
    R::FName flashlightClassName{0, 0}; // FName of that UClass (= "prop_equipment_flashlight_C")
    void* mainPlayerClass = nullptr;
    void* addPropToPlayerFn = nullptr;
};
Cache g_cache;

bool EnsureReflectionResolved(void* mainPlayer) {
    if (!mainPlayer) return false;

    if (g_cache.mainGameInstance && !R::IsLive(g_cache.mainGameInstance))
        g_cache.mainGameInstance = nullptr;
    if (!g_cache.mainGameInstance) {
        g_cache.mainGameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        if (!g_cache.mainGameInstance) {
            UE_LOGW("flashlight_setup: mainGameInstance_C not yet alive");
            return false;
        }
    }
    if (g_cache.saveGameInstOff < 0) {
        void* giClass = R::ClassOf(g_cache.mainGameInstance);
        if (giClass) g_cache.saveGameInstOff = R::FindPropertyOffset(giClass, L"save_gameInst");
        if (g_cache.saveGameInstOff < 0) {
            UE_LOGW("flashlight_setup: save_gameInst offset not found on mainGameInstance");
            return false;
        }
    }
    if (!g_cache.saveSlotClass) {
        g_cache.saveSlotClass = R::FindClass(P::name::SaveSlotClass);
        if (!g_cache.saveSlotClass) {
            UE_LOGW("flashlight_setup: saveSlot_C class not yet alive");
            return false;
        }
    }
    if (g_cache.batteryOff < 0)
        g_cache.batteryOff = R::FindPropertyOffset(g_cache.saveSlotClass, L"battery");
    if (g_cache.flashlightBatteryOff < 0)
        g_cache.flashlightBatteryOff =
            R::FindPropertyOffset(g_cache.saveSlotClass, L"flashlightBattery");
    if (g_cache.batteryOff < 0 || g_cache.flashlightBatteryOff < 0) {
        UE_LOGW("flashlight_setup: saveSlot offsets unresolved "
                "(battery=%d flashlightBattery=%d)",
                g_cache.batteryOff, g_cache.flashlightBatteryOff);
        return false;
    }
    if (!g_cache.battsClass) {
        g_cache.battsClass = R::FindClass(P::name::BattsClass);
        if (!g_cache.battsClass) {
            UE_LOGW("flashlight_setup: prop_batts_C class not yet alive");
            return false;
        }
    }
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

void* GetLiveSaveSlot() {
    if (!g_cache.mainGameInstance || g_cache.saveGameInstOff < 0) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_cache.mainGameInstance) + g_cache.saveGameInstOff);
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
    void* slot = GetLiveSaveSlot();
    if (!slot) {
        UE_LOGW("flashlight_setup: SetBatteryFull: saveSlot null (save not loaded yet)");
        return false;
    }
    auto* slotBytes = reinterpret_cast<uint8_t*>(slot);
    const float prevBattery = *reinterpret_cast<float*>(slotBytes + g_cache.batteryOff);
    void* prevBatteryClass = *reinterpret_cast<void**>(
        slotBytes + g_cache.flashlightBatteryOff);

    // 2026-05-26: hands-on observation showed VOTV's battery scale is
    // 0-100 (percentage), NOT 0-1. Reading a fresh s_may2026 save returns
    // ~98.68. The earlier 1.0 default was draining the battery to "1%"!
    // 100.0 = full charge.
    *reinterpret_cast<float*>(slotBytes + g_cache.batteryOff) = 100.f;
    *reinterpret_cast<void**>(slotBytes + g_cache.flashlightBatteryOff) = g_cache.battsClass;

    UE_LOGI("flashlight_setup: SetBatteryFull: saveSlot=%p battery %.2f->100.00 "
            "flashlightBattery %p->%p (prop_batts_C)",
            slot, prevBattery, prevBatteryClass, g_cache.battsClass);
    return true;
}

bool EnsureFlashlightReady(void* mainPlayer) {
    if (!EnsureReflectionResolved(mainPlayer)) return false;

    // Read current state: hasFlashlight + saveSlot.battery + flashlightBattery.
    const bool hadFlashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(mainPlayer) + P::off::AmainPlayer_hasFlashlight);
    const bool crankFlashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(mainPlayer) + P::off::AmainPlayer_crankFlashlight);

    void* slot = GetLiveSaveSlot();
    float battery = -1.f;
    void* batteryClass = nullptr;
    if (slot) {
        battery = *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(slot) + g_cache.batteryOff);
        batteryClass = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(slot) + g_cache.flashlightBatteryOff);
    }
    UE_LOGI("flashlight_setup: pre-state: hasFlashlight=%d crank=%d battery=%.2f "
            "batteryClass=%p saveSlot=%p",
            hadFlashlight ? 1 : 0, crankFlashlight ? 1 : 0, battery, batteryClass, slot);

    // Give the flashlight if missing. addPropToPlayer is idempotent in
    // intent (cheat menu calls it freely) but may stack duplicates --
    // skip the call if hasFlashlight is already true so we don't
    // accumulate items.
    if (!hadFlashlight) {
        GiveFlashlight(mainPlayer);
    } else {
        UE_LOGI("flashlight_setup: hasFlashlight already true -- skipping GiveFlashlight");
    }

    // Always top off the battery -- save may have a discharged one.
    SetBatteryFull(mainPlayer);

    // Re-read for post-state log.
    const bool nowHasFlashlight = *reinterpret_cast<bool*>(
        reinterpret_cast<uint8_t*>(mainPlayer) + P::off::AmainPlayer_hasFlashlight);
    float nowBattery = -1.f;
    if (slot) {
        nowBattery = *reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(slot) + g_cache.batteryOff);
    }
    UE_LOGI("flashlight_setup: post-state: hasFlashlight=%d battery=%.2f -- ready=%d",
            nowHasFlashlight ? 1 : 0, nowBattery,
            (nowHasFlashlight && nowBattery > 1.f) ? 1 : 0);
    return nowHasFlashlight && nowBattery > 1.f;
}

}  // namespace coop::dev::flashlight_setup
