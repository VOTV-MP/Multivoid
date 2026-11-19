// ue_wrap/core/process_launch.cpp -- see ue_wrap/core/process_launch.h.

#include "ue_wrap/core/process_launch.h"

#include "ue_wrap/core/hook.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>

namespace ue_wrap::process_launch {
namespace {

using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);

CreateProcessWFn g_trampoline = nullptr;
std::atomic<BeforeLaunch> g_before{nullptr};
std::atomic<bool> g_installed{false};

BOOL WINAPI Detour(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa, LPSECURITY_ATTRIBUTES ta, BOOL inherit, DWORD flags,
                   LPVOID env, LPCWSTR dir, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi) {
    if (const BeforeLaunch fn = g_before.load(std::memory_order_acquire)) fn(app, cmd);
    return g_trampoline(app, cmd, pa, ta, inherit, flags, env, dir, si, pi);
}

}  // namespace

bool Install(BeforeLaunch fn) {
    g_before.store(fn, std::memory_order_release);
    if (g_installed.load(std::memory_order_acquire)) return true;
    HMODULE const k32 = ::GetModuleHandleW(L"kernel32.dll");
    void* const target = k32 ? reinterpret_cast<void*>(::GetProcAddress(k32, "CreateProcessW")) : nullptr;
    if (!target || !ue_wrap::hook::Init() ||
        !ue_wrap::hook::Install(target, reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_trampoline))) {
        UE_LOGW("process_launch: the CreateProcessW hook could not be made (target %p)", target);
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    UE_LOGI("process_launch: CreateProcessW is watched");
    return true;
}

}  // namespace ue_wrap::process_launch
