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
void* g_clear = nullptr;
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
    if (g_library && g_clear) return true;
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextTryMs) return false;
    g_nextTryMs = now + 1000;
    if (!g_library) g_library = R::FindClassDefaultObject(L"KismetSystemLibrary");
    void* cls = g_library ? R::FindClass(L"KismetSystemLibrary") : nullptr;
    if (!cls) return false;
    if (!g_clear) g_clear = R::FindFunction(cls, L"K2_ClearTimer");
    return g_clear != nullptr;
}

bool Clear(void* object, const wchar_t* function) { return Run(g_clear, object, function); }

}  // namespace ue_wrap::kismet_timer
