// ue_wrap/devices/base_window.cpp -- see ue_wrap/devices/base_window.h. Engine access for VOTV's
// base observation window (AbaseWindow_C).
//
// Offsets are resolved from the live class by member name (FindPropertyOffset). A member the name
// does not find leaves its lane off, said once; no offset is guessed.

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
// the fully-written caches below. Game-thread writes; the wipe watch's and the apply's reads.
std::atomic<bool> g_resolved{false};

void*   g_winCls     = nullptr;  // baseWindow_C UClass
int32_t g_cleanOff   = -1;       // AbaseWindow_C::clean
int32_t g_keyOff     = -1;       // Aactor_save_C::Key
void*   g_setCleanFn = nullptr;  // AbaseWindow_C::setClean() -- SetCustomPrimitiveDataFloat(0, clean)

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* winCls = R::FindClass(L"baseWindow_C");
    if (!winCls) return false;  // BP class not loaded yet -- caller retries

    // `clean` is declared on AbaseWindow_C itself.
    const int32_t cleanOff = R::FindPropertyOffset(winCls, L"clean");
    if (cleanOff < 0) UE_LOGE("base_window: 'clean' not found by name -- window wipes stay off");
    // `Key` is declared on the Aactor_save_C parent, which the property lookup climbs to.
    const int32_t keyOff = R::FindPropertyOffset(winCls, L"Key");
    if (keyOff < 0) UE_LOGE("base_window: 'Key' not found by name -- windows carry no key, so none is synced");
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
