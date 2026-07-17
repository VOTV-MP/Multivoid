// ue_wrap/vitals.cpp -- see ue_wrap/vitals.h.

#include "ue_wrap/actors/vitals.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <cstdint>

namespace ue_wrap::vitals {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;

// One-time resolution cache. History (from restore_vitals.cpp, which used to own
// this): pre-cache every access walked GUObjectArray twice + several
// FindPropertyOffset = ~100-300 ms of game-thread block per call -> a visible FPS
// hitch. Cached, the steady-state cost is one pointer deref + one float access.
// Cleared / re-resolved only if the cached GameInstance fails IsLive (level
// transition / hot reload); the GameInstance singleton otherwise never dies.
struct Cache {
    void* gameInstance = nullptr;        // live UmainGameInstance_C*
    int32_t saveGameInstOff = -1;        // mainGameInstance_C::save_gameInst (UsaveSlot_C*)
    void* saveSlotClass = nullptr;       // UClass* for UsaveSlot_C (offset-lookup target)
    int32_t fieldOff[4] = {-1, -1, -1, -1};  // indexed by Field
};
Cache g_cache;

const wchar_t* FieldName(Field f) {
    switch (f) {
        case Field::Health:    return L"health";
        case Field::MaxHealth: return L"maxHealth";
        case Field::Food:      return L"food";
        case Field::Sleep:     return L"sleep";
    }
    return L"";
}

// Resolve GameInstance + the save_gameInst offset + the saveSlot UClass. Returns
// false if any step isn't up yet. Game-thread only.
bool EnsureBase() {
    if (g_cache.gameInstance && !R::IsLive(g_cache.gameInstance)) {
        g_cache.gameInstance = nullptr;  // stale after a level transition
    }
    if (!g_cache.gameInstance) {
        g_cache.gameInstance = R::FindObjectByClass(P::name::GameInstanceClass);
        if (!g_cache.gameInstance) return false;
    }
    if (g_cache.saveGameInstOff < 0) {
        void* giClass = R::ClassOf(g_cache.gameInstance);
        if (!giClass) return false;
        g_cache.saveGameInstOff = R::FindPropertyOffset(giClass, L"save_gameInst");
        if (g_cache.saveGameInstOff < 0) return false;
    }
    if (!g_cache.saveSlotClass) {
        g_cache.saveSlotClass = R::FindClass(P::name::SaveSlotClass);
        if (!g_cache.saveSlotClass) return false;
    }
    return true;
}

// The canonical live saveSlot pointer (mainGameInstance.save_gameInst). null if
// the save isn't registered yet. Pointing THROUGH the GameInstance (rather than
// FindObjectByClass(saveSlot_C), which walks GUObjectArray and can surface a
// stale menu-era UsaveSlot_C from ui_saveSlots arrays) is the unambiguous path.
void* ResolveSlot() {
    if (!EnsureBase()) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(g_cache.gameInstance) + g_cache.saveGameInstOff);
}

int32_t ResolveFieldOffset(Field f) {
    const int idx = static_cast<int>(f);
    if (g_cache.fieldOff[idx] < 0 && g_cache.saveSlotClass) {
        g_cache.fieldOff[idx] = R::FindPropertyOffset(g_cache.saveSlotClass, FieldName(f));
    }
    return g_cache.fieldOff[idx];
}

}  // namespace

bool Read(Field f, float* out) {
    void* slot = ResolveSlot();
    if (!slot) return false;
    const int32_t off = ResolveFieldOffset(f);
    if (off < 0) return false;
    if (out) *out = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + off);
    return true;
}

bool Write(Field f, float v) {
    void* slot = ResolveSlot();
    if (!slot) return false;
    const int32_t off = ResolveFieldOffset(f);
    if (off < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(slot) + off) = v;
    return true;
}

}  // namespace ue_wrap::vitals
