// ue_wrap/save_capture.cpp -- see header.

#include "ue_wrap/engine/save_capture.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::save_capture {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

namespace {

// Dispatch a no-argument mainGamemode UFunction by name. Returns false (logged) if
// the function is unresolved or the ProcessEvent dispatch fails.
bool CallGmVoid(void* gm, void* gmCls, const wchar_t* fnName) {
    void* fn = R::FindFunction(gmCls, fnName);
    if (!fn) {
        UE_LOGW("save_capture: mainGamemode.%ls unresolved", fnName);
        return false;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    return ue_wrap::Call(gm, f);
}

}  // namespace

bool CaptureLiveWorldToScratchSlot(const std::wstring& scratchSlotName) {
    UE_ASSERT_GAME_THREAD("save_capture::CaptureLiveWorldToScratchSlot");
    if (scratchSlotName.empty()) return false;

    // 1. The host gamemode owns the live world + the save container.
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    void* gmCls = gm ? R::ClassOf(gm) : nullptr;
    if (!gm || !gmCls) {
        UE_LOGW("save_capture: mainGamemode not live -- cannot capture host world");
        return false;
    }

    // 2. The world save container the populate writes into. Read it BEFORE the
    //    populate so we can probe objectsData's count delta. That delta is also the
    //    one safety check this design needs: saveObjects MUST rebuild objectsData
    //    (not append) for a standalone call to be correct. Evidence it rebuilds:
    //    saveObjects builds a local 'copy1' struct_save array then assigns it to the
    //    member -- the build-local-then-replace pattern (UE4SS_ObjectDump_GAMEPLAY_
    //    SAVE.txt:275639) -- and SP save/reload never duplicates the world. The probe
    //    below confirms it live; a >1.5x growth would mean it appends (then we'd need
    //    to clear first). The failure mode is host-safe either way (a doubled scratch
    //    blob only over-populates the JOINER; the host's slot is never written).
    void* saveSlot = *reinterpret_cast<void* const*>(
        reinterpret_cast<const uint8_t*>(gm) + P::off::AmainGamemode_saveSlot);
    if (!saveSlot) {
        UE_LOGW("save_capture: mainGamemode.saveSlot is null -- nothing to serialize");
        return false;
    }
    // objectsData is a UE TArray {Data@0x0, Num@0x8, Max@0xC}; read its Num.
    auto objectsDataNum = [saveSlot]() -> int32_t {
        return *reinterpret_cast<const int32_t*>(
            reinterpret_cast<const uint8_t*>(saveSlot) + P::off::UsaveSlot_objectsData + 0x8);
    };
    const int32_t objCountBefore = objectsDataNum();

    // 3. Repopulate the in-memory world save from LIVE actors. saveObjects is the
    //    critical step: it walks every int_save_C world actor (props + NPCs, incl.
    //    a turned-on kerfur, which serializes as its live NPC state -- exactly what
    //    an SP save/reload restores). saveTriggers refreshes door/light/keypad
    //    states. We deliberately SKIP the player-state populates (playerTransform/
    //    inventory/heldObj): the joiner overrides those with its own per-player
    //    state and each has a live coop sync channel. These are pure read-into-array
    //    populates -- no actor mutation, no disk write, no save event -- so nothing
    //    "real" happens to the host's session here.
    {
        void* fn = R::FindFunction(gmCls, P::name::MainGamemodeSaveObjectsFn);
        if (!fn) {
            UE_LOGW("save_capture: mainGamemode.saveObjects unresolved -- abort "
                    "(serializing now would ship a stale object list)");
            return false;
        }
        ue_wrap::ParamFrame f(fn);
        if (!f.valid()) return false;
        // saveObjects(bool quicksave): the zeroed frame leaves quicksave=false (the
        // full populate, matching a normal save) -- exactly what we want.
        if (!ue_wrap::Call(gm, f)) {
            UE_LOGW("save_capture: mainGamemode.saveObjects dispatch failed -- abort");
            return false;
        }
    }
    CallGmVoid(gm, gmCls, P::name::MainGamemodeSaveTriggersFn);  // best-effort; objects are the critical half

    // Safety probe: confirm saveObjects rebuilt (didn't append) objectsData.
    const int32_t objCountAfter = objectsDataNum();
    if (objCountBefore > 0 && objCountAfter > objCountBefore + objCountBefore / 2) {
        UE_LOGW("save_capture: objectsData grew %d -> %d (>1.5x) -- saveObjects appears to APPEND, "
                "not rebuild; the scratch blob may carry DUPLICATE objects. Investigate before "
                "trusting this build's live capture.", objCountBefore, objCountAfter);
    } else {
        UE_LOGI("save_capture: objectsData repopulated %d -> %d live world object(s)",
                objCountBefore, objCountAfter);
    }

    // 4. Serialize it to a SCRATCH slot. GameplayStatics::SaveGameToSlot(obj, slot,
    //    idx): the slot NAME is our parameter, so the host's canonical slot is never
    //    named or touched. On the host SaveGameToSlot is un-hooked (coop::save_block
    //    installs on clients only), so this runs as the stock engine serializer.
    void* gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    void* gsCls = gsCdo ? R::ClassOf(gsCdo) : nullptr;
    void* saveFn = gsCls ? R::FindFunction(gsCls, P::name::SaveGameToSlotFn) : nullptr;
    if (!gsCdo || !saveFn) {
        UE_LOGW("save_capture: GameplayStatics::SaveGameToSlot unresolved -- cannot serialize");
        return false;
    }

    std::wstring slotBuf = scratchSlotName;  // kept alive across the Call (the FString aliases it)
    R::FString fs{};
    fs.Data = slotBuf.data();
    fs.Num  = static_cast<int32_t>(slotBuf.size()) + 1;  // FString::Num counts the null terminator
    fs.Max  = fs.Num;

    ue_wrap::ParamFrame f(saveFn);
    if (!f.valid()) return false;
    f.Set<void*>(L"SaveGameObject", saveSlot);
    f.SetRaw(L"SlotName", &fs, sizeof(fs));
    f.Set<int32_t>(L"UserIndex", 0);
    if (!ue_wrap::Call(gsCdo, f)) {
        UE_LOGW("save_capture: SaveGameToSlot dispatch failed");
        return false;
    }
    const bool ok = f.Get<uint8_t>(L"ReturnValue") != 0;
    if (ok) {
        UE_LOGI("save_capture: host world serialized LIVE to scratch slot '%ls' "
                "(objects+triggers repopulated; canonical slot untouched)",
                scratchSlotName.c_str());
    } else {
        UE_LOGW("save_capture: SaveGameToSlot returned false for scratch slot '%ls'",
                scratchSlotName.c_str());
    }
    return ok;
}

}  // namespace ue_wrap::save_capture
