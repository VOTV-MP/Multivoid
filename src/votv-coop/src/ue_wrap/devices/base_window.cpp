// ue_wrap/base_window.cpp -- see ue_wrap/base_window.h. Engine access for VOTV's
// base observation window (AbaseWindow_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset) with
// the documented Alpha 0.9.0-n CXX-dump values as a logged fallback, matching the
// door wrapper (version-tagging rule -- offsets stay correct across game recooks).

#include "ue_wrap/devices/base_window.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::base_window {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only. Published via the g_resolved
// release-store / acquire-load so any thread that sees g_resolved==true also sees
// the fully-written caches below. Game-thread writes; poll/apply reads.
std::atomic<bool> g_resolved{false};

void*   g_winCls     = nullptr;  // baseWindow_C UClass
int32_t g_cleanOff   = -1;       // AbaseWindow_C::clean  (Alpha 0.9.0-n: 0x0260)
int32_t g_keyOff     = -1;       // Aactor_save_C::Key    (Alpha 0.9.0-n: 0x0230)
void*   g_setCleanFn = nullptr;  // AbaseWindow_C::setClean() -- SetCustomPrimitiveDataFloat(0, clean)

// Documented Alpha 0.9.0-n fallbacks (CXXHeaderDump/baseWindow.hpp + actor_save.hpp).
constexpr int32_t kCleanOffFallback = 0x0260;
constexpr int32_t kKeyOffFallback   = 0x0230;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* winCls = R::FindClass(L"baseWindow_C");
    if (!winCls) return false;  // BP class not loaded yet -- caller retries

    // `clean` is declared on AbaseWindow_C itself.
    int32_t cleanOff = R::FindPropertyOffset(winCls, L"clean");
    if (cleanOff < 0) {
        UE_LOGW("base_window: reflected clean offset not found -- using fallback 0x%04X", kCleanOffFallback);
        cleanOff = kCleanOffFallback;
    }
    // `Key` is declared on the Aactor_save_C parent; FindPropertyOffset does NOT climb
    // to super, so query the declaring class explicitly (same pattern as door/triggerBase).
    int32_t keyOff = -1;
    if (void* saveCls = R::FindClass(L"actor_save_C")) {
        keyOff = R::FindPropertyOffset(saveCls, L"Key");
    }
    if (keyOff < 0) {
        UE_LOGW("base_window: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    void* setCleanFn = R::FindFunction(winCls, L"setClean");
    if (!setCleanFn)
        UE_LOGW("base_window: setClean UFunction not found -- a mirrored clean won't repaint the shader");

    g_winCls     = winCls;
    g_cleanOff   = cleanOff;
    g_keyOff     = keyOff;
    g_setCleanFn = setCleanFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("base_window: resolved baseWindow_C=%p clean@0x%04X Key@0x%04X setClean=%p",
            winCls, cleanOff, keyOff, setCleanFn);
    return true;
}

void* BaseWindowClass() { return g_winCls; }

bool IsBaseWindow(void* obj) {
    if (!obj || !g_winCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_winCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* win) {
    if (!win || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(win) + g_keyOff);
    return R::ToString(key);
}

bool ReadClean(void* win, float& out) {
    if (!win || g_cleanOff < 0) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(win) + g_cleanOff);
    return true;
}

bool WriteCleanAndApply(void* win, float clean) {
    if (!win || !g_resolved.load(std::memory_order_acquire) || g_cleanOff < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(win) + g_cleanOff) = clean;
    // setClean() has ZERO BP input parameters (RE + CXXHeaderDump/baseWindow.hpp: `void
    // setClean();` -- it reads this->clean directly and does StaticMesh1.
    // SetCustomPrimitiveDataFloat(0, clean)). So the field write ABOVE must precede this call;
    // the zeroed no-arg ParamFrame is CORRECT, not a missing argument. No-op-safe if unresolved
    // (the field write alone is the truth; the repaint just won't show until the next native
    // setClean), but it resolves on every real build.
    if (g_setCleanFn) {
        ParamFrame f(g_setCleanFn);
        if (f.valid()) Call(win, f);
    }
    return true;
}

}  // namespace ue_wrap::base_window
