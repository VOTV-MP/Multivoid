// ue_wrap/core/kismet_timer.cpp -- see ue_wrap/core/kismet_timer.h.

#include "ue_wrap/core/kismet_timer.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdint>
#include <cwchar>

namespace ue_wrap::kismet_timer {
namespace {

namespace R = ue_wrap::reflection;

void* g_library = nullptr;
void* g_pause = nullptr;
void* g_unpause = nullptr;
void* g_isPaused = nullptr;
uint64_t g_nextTryMs = 0;

// Sets up the call on `f`; false when the frame or the name would not take.
bool Prepare(ParamFrame& f, void* object, const wchar_t* function) {
    if (!f.valid()) return false;
    f.Set<void*>(L"Object", object);
    const int32_t num = static_cast<int32_t>(std::wcslen(function)) + 1;
    struct { const wchar_t* data; int32_t n; int32_t m; } name{function, num, num};
    return f.SetRaw(L"FunctionName", &name, sizeof(name));
}

bool Run(void* fn, void* object, const wchar_t* function) {
    if (!object || !function || !Resolved()) return false;
    ParamFrame f(fn);
    return Prepare(f, object, function) && Call(g_library, f);
}

}  // namespace

bool Resolved() {
    if (g_library && g_pause && g_unpause && g_isPaused) return true;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;
    if (!g_library) g_library = R::FindClassDefaultObject(L"KismetSystemLibrary");
    void* cls = g_library ? R::FindClass(L"KismetSystemLibrary") : nullptr;
    if (!cls) return false;
    if (!g_pause) g_pause = R::FindFunction(cls, L"K2_PauseTimer");
    if (!g_unpause) g_unpause = R::FindFunction(cls, L"K2_UnPauseTimer");
    if (!g_isPaused) g_isPaused = R::FindFunction(cls, L"K2_IsTimerPaused");
    return g_pause && g_unpause && g_isPaused;
}

bool Pause(void* object, const wchar_t* function) { return Run(g_pause, object, function); }
bool UnPause(void* object, const wchar_t* function) { return Run(g_unpause, object, function); }

bool IsPaused(void* object, const wchar_t* function) {
    if (!object || !function || !Resolved()) return false;
    ParamFrame f(g_isPaused);
    return Prepare(f, object, function) && Call(g_library, f) && f.Get<bool>(L"ReturnValue");
}

}  // namespace ue_wrap::kismet_timer
