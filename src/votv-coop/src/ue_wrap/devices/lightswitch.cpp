// ue_wrap/lightswitch.cpp -- see ue_wrap/lightswitch.h. Engine access for VOTV
// light groups (Atrigger_lightRoot_C). Offsets resolved from the live class via
// reflection (version-portable); the Alpha 0.9.0-n values are logged fallbacks.

#include "ue_wrap/devices/lightswitch.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::lightswitch {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_rootCls    = nullptr;  // trigger_lightRoot_C UClass
int32_t g_keyOff     = -1;       // AtriggerBase_C::Key       (Alpha 0.9.0-n: 0x0260)
int32_t g_isActiveOff = -1;      // trigger_lightRoot_C::IsActive (0x02B8)
void*   g_setActiveFn = nullptr; // SetActive(bool Active)

constexpr int32_t kKeyOffFallback      = 0x0260;
constexpr int32_t kIsActiveOffFallback = 0x02B8;

// --- The light SWITCH (Alightswitch_C) ---
std::atomic<bool> g_swResolved{false};
void*   g_swCls   = nullptr;  // lightswitch_C UClass
int32_t g_swKeyOff = -1;      // AtriggerBase_C::Key (shared base offset, 0x0260)
int32_t g_swAOff  = -1;       // Alightswitch_C::A (the flip bool, 0x02A0)
void*   g_useFn   = nullptr;  // use()
constexpr int32_t kSwitchAOffFallback = 0x02A0;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* rootCls = R::FindClass(L"trigger_lightRoot_C");
    if (!rootCls) return false;

    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) {
        keyOff = R::FindPropertyOffset(trigCls, L"Key");
    }
    if (keyOff < 0) {
        UE_LOGW("lightswitch: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    int32_t isActiveOff = R::FindPropertyOffset(rootCls, L"IsActive");
    if (isActiveOff < 0) {
        UE_LOGW("lightswitch: reflected IsActive offset not found -- using fallback 0x%04X", kIsActiveOffFallback);
        isActiveOff = kIsActiveOffFallback;
    }
    void* setActiveFn = R::FindFunction(rootCls, L"SetActive");
    if (!setActiveFn) {
        UE_LOGW("lightswitch: SetActive UFunction not found -- not ready");
        return false;
    }

    g_rootCls     = rootCls;
    g_keyOff      = keyOff;
    g_isActiveOff = isActiveOff;
    g_setActiveFn = setActiveFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved trigger_lightRoot_C=%p Key@0x%04X IsActive@0x%04X SetActive=%p",
            rootCls, keyOff, isActiveOff, setActiveFn);
    return true;
}

void* SetActiveFn() { return g_setActiveFn; }

bool IsLightRoot(void* obj) {
    if (!obj || !g_rootCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_rootCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* root) {
    if (!root || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(root) + g_keyOff);
    return R::ToString(key);
}

bool TryReadActive(void* root, bool& on) {
    if (!root || g_isActiveOff < 0) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(root) + g_isActiveOff);
    return true;
}

bool CallSetActive(void* root, bool on) {
    if (!root || !g_setActiveFn) return false;
    ParamFrame f(g_setActiveFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Active", on);
    return Call(root, f);
}

// --- The light SWITCH (Alightswitch_C) ---

bool EnsureSwitchResolved() {
    if (g_swResolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"lightswitch_C");
    if (!cls) return false;
    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) keyOff = R::FindPropertyOffset(trigCls, L"Key");
    if (keyOff < 0) keyOff = kKeyOffFallback;
    int32_t aOff = R::FindPropertyOffset(cls, L"A");
    if (aOff < 0) {
        UE_LOGW("lightswitch: switch A offset not found -- using fallback 0x%04X", kSwitchAOffFallback);
        aOff = kSwitchAOffFallback;
    }
    void* useFn = R::FindFunction(cls, L"use");
    if (!useFn) { UE_LOGW("lightswitch: switch use() UFunction not found -- not ready"); return false; }
    g_swCls = cls; g_swKeyOff = keyOff; g_swAOff = aOff; g_useFn = useFn;
    g_swResolved.store(true, std::memory_order_release);
    UE_LOGI("lightswitch: resolved switch lightswitch_C=%p Key@0x%04X A@0x%04X use=%p",
            cls, keyOff, aOff, useFn);
    return true;
}

bool IsLightSwitch(void* obj) {
    if (!obj || !g_swCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_swCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetSwitchKeyString(void* sw) {
    if (!sw || g_swKeyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(sw) + g_swKeyOff);
    return R::ToString(key);
}

bool TryReadSwitchA(void* sw, bool& on) {
    if (!sw || g_swAOff < 0) return false;
    on = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(sw) + g_swAOff);
    return true;
}

bool CallUse(void* sw) {
    if (!sw || !g_useFn) return false;
    ParamFrame f(g_useFn);
    if (!f.valid()) return false;
    return Call(sw, f);
}

}  // namespace ue_wrap::lightswitch
