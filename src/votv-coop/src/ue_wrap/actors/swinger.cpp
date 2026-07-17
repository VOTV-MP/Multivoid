// ue_wrap/swinger.cpp -- see ue_wrap/swinger.h. Engine access for VOTV container
// lids (Aprop_swinger_C). The `opened` offset is resolved from the live class via
// reflection (version-portable); the Alpha 0.9.0-n value is a logged fallback.
// The Key is read through ue_wrap::prop (a swinger IS an Aprop_C).

#include "ue_wrap/actors/swinger.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::swinger {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

void*   g_swingerCls = nullptr;  // prop_swinger_C UClass
int32_t g_openedOff  = -1;       // Aprop_swinger_C::opened (Alpha 0.9.0-n: 0x03C5)
void*   g_openFn     = nullptr;  // Open(bool Damage)
void*   g_closeFn    = nullptr;  // Close()

constexpr int32_t kOpenedOffFallback = 0x03C5;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"prop_swinger_C");
    if (!cls) return false;

    int32_t openedOff = R::FindPropertyOffset(cls, L"opened");
    if (openedOff < 0) {
        UE_LOGW("swinger: reflected opened offset not found -- using fallback 0x%04X", kOpenedOffFallback);
        openedOff = kOpenedOffFallback;
    }
    void* openFn  = R::FindFunction(cls, L"Open");
    void* closeFn = R::FindFunction(cls, L"Close");
    if (!openFn || !closeFn) {
        UE_LOGW("swinger: UFunction resolve incomplete (Open=%p Close=%p) -- not ready", openFn, closeFn);
        return false;
    }

    g_swingerCls = cls;
    g_openedOff  = openedOff;
    g_openFn     = openFn;
    g_closeFn    = closeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("swinger: resolved prop_swinger_C=%p opened@0x%04X Open=%p Close=%p",
            cls, openedOff, openFn, closeFn);
    return true;
}

void* OpenFn() { return g_openFn; }
void* CloseFn() { return g_closeFn; }

bool IsSwinger(void* obj) {
    if (!obj || !g_swingerCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_swingerCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool TryReadOpen(void* swinger, bool& on) {
    if (!swinger || g_openedOff < 0) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(swinger) + g_openedOff);
    return true;
}

bool CallOpen(void* swinger, bool damage) {
    if (!swinger || !g_openFn) return false;
    ParamFrame f(g_openFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Damage", damage);
    return Call(swinger, f);
}

bool CallClose(void* swinger) {
    if (!swinger || !g_closeFn) return false;
    ParamFrame f(g_closeFn);
    if (!f.valid()) return false;
    return Call(swinger, f);
}

}  // namespace ue_wrap::swinger
