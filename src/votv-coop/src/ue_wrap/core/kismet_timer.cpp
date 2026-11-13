// ue_wrap/core/kismet_timer.cpp -- see ue_wrap/core/kismet_timer.h.

#include "ue_wrap/core/kismet_timer.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>

namespace ue_wrap::kismet_timer {
namespace {

namespace R = ue_wrap::reflection;

void* g_library = nullptr;
void* g_clear = nullptr;
uint64_t g_nextLogMs = 0;

// Sets up the call on `f`; false when the frame or the name would not take.
bool Prepare(ParamFrame& f, void* object, const wchar_t* function) {
    if (!f.valid()) return false;
    f.Set<void*>(L"Object", object);
    const int32_t num = static_cast<int32_t>(std::wcslen(function)) + 1;
    struct { const wchar_t* data; int32_t n; int32_t m; } name{function, num, num};
    return f.SetRaw(L"FunctionName", &name, sizeof(name));
}

}  // namespace

// Every call attempts the lookup until it succeeds, so no caller reads false for another caller's failed attempt; only the
// failure line is limited to one a second.
bool Resolved() {
    if (g_library && g_clear) return true;
    if (!g_library) g_library = R::FindClassDefaultObject(L"KismetSystemLibrary");
    void* cls = g_library ? R::FindClass(L"KismetSystemLibrary") : nullptr;
    if (cls && !g_clear) g_clear = R::FindFunction(cls, L"K2_ClearTimer");
    if (g_library && g_clear) return true;
    const uint64_t now = ::GetTickCount64();
    if (now >= g_nextLogMs) {
        g_nextLogMs = now + 1000;
        UE_LOGW("kismet_timer: KismetSystemLibrary or K2_ClearTimer not found (library=%p, function=%p)", g_library, g_clear);
    }
    return false;
}

bool Clear(void* object, const wchar_t* function) {
    if (!object || !function || !Resolved()) return false;
    ParamFrame f(g_clear);
    return Prepare(f, object, function) && Call(g_library, f);
}

}  // namespace ue_wrap::kismet_timer
