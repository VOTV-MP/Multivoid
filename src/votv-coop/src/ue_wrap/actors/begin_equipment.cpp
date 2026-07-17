// ue_wrap/begin_equipment.cpp -- see ue_wrap/begin_equipment.h.

#include "ue_wrap/actors/begin_equipment.h"

#include "ue_wrap/engine/engine.h"      // SpawnActor, FVector
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <cstring>

#include <windows.h>             // SEH (__try/__except, EXCEPTION_EXECUTE_HANDLER)

namespace ue_wrap::begin_equipment {

namespace R = ue_wrap::reflection;

namespace {

// SEH-only frame: NO C++ destructors in scope (the game_thread.cpp doctrine -- __try/__except can't
// share a function with an unwinding object). Does the raw spawn -> getData -> AddEquipment ->
// destroy through reflection. Returns 1 (AddEquipment true), 0 (false / spawn null), or -1 (faulted).
int GiveFromClassRaw(void* cls, void* gm, void* getDataFn, void* addFn, void* destroyFn) {
    __try {
        // Spawn a throwaway instance to read its CANONICAL save record -- exactly what the gamemode's
        // begin-equipment loop does (`<prop>.getData()`); the record + fresh key are the game's own.
        void* prop = ue_wrap::engine::SpawnActor(cls, FVector{0.f, 0.f, 0.f});
        if (!prop) return 0;
        uint8_t data[0x100] = {};                 // Fstruct_save is 0x100 (saveSlot.hpp)
        const bool got = R::CallFunction(prop, getDataFn, data);
        int added = 0;
        if (got) {
            // AddEquipment(Fstruct_save Data, bool& return): Data @ 0 (0x100), return bool @ 0x100.
            uint8_t frame[0x108] = {};
            std::memcpy(frame, data, sizeof(data));
            if (R::CallFunction(gm, addFn, frame)) added = (frame[0x100] != 0) ? 1 : 0;
        }
        if (destroyFn) R::CallFunction(prop, destroyFn, nullptr);  // after AddEquipment (record may ref the live prop)
        return got ? added : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

}  // namespace

bool GiveFromClass(const std::wstring& className) {
    void* cls   = R::FindClass(className.c_str());
    void* gm    = R::FindObjectByClass(L"mainGamemode_C");
    void* gmCls = R::FindClass(L"mainGamemode_C");
    void* addFn     = gmCls ? R::FindFunction(gmCls, L"AddEquipment") : nullptr;
    void* getDataFn = cls   ? R::FindFunction(cls,   L"getData")      : nullptr;
    if (!cls || !gm || !addFn || !getDataFn) {
        UE_LOGW("begin_equipment: '%ls' -- cls=%p gm=%p AddEquipment=%p getData=%p (cannot equip)",
                className.c_str(), cls, gm, addFn, getDataFn);
        return false;
    }
    void* destroyFn = R::FindFunction(cls, L"K2_DestroyActor");
    const int r = GiveFromClassRaw(cls, gm, getDataFn, addFn, destroyFn);
    if (r < 0) {
        UE_LOGW("begin_equipment: equip '%ls' FAULTED (SEH) -- skipped (no crash)", className.c_str());
        return false;
    }
    UE_LOGI("begin_equipment: equip '%ls' via getData->AddEquipment -> %s",
            className.c_str(), r > 0 ? "true" : "false");
    return r > 0;
}

}  // namespace ue_wrap::begin_equipment
