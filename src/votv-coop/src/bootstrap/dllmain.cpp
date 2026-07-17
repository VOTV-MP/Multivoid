// votv-coop standalone bootstrap.
//
// Entry point for the STANDALONE mod DLL (RULE No.3 -- no UE4SS at runtime).
// At this stage it only proves the loader + build pipeline: on load it
// writes a marker file next to itself so we can confirm the DLL was mapped
// into VotV-Win64-Shipping.exe without any UE4SS involvement. Reflection
// (resolve GUObjectArray/GNames/ProcessEvent via AOB sigs) and hooking land
// in later steps, behind ue_wrap/.

#include "coop/session/shutdown.h"
#include "coop/version.h"
#include "harness/harness.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

namespace {

void WriteMarker() {
    // Locate this DLL on disk so the marker lands next to it.
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&WriteMarker), &self);

    wchar_t dllPath[MAX_PATH] = {};
    ::GetModuleFileNameW(self, dllPath, MAX_PATH);

    // Replace the filename component with our marker name.
    wchar_t* lastSep = nullptr;
    for (wchar_t* p = dllPath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') lastSep = p;
    }
    wchar_t markerPath[MAX_PATH] = {};
    if (lastSep) {
        const size_t dirLen = static_cast<size_t>(lastSep - dllPath) + 1;
        wcsncpy_s(markerPath, dllPath, dirLen);
    }
    wcscat_s(markerPath, L"votv-coop-loaded.txt");

    FILE* f = nullptr;
    if (_wfopen_s(&f, markerPath, L"a") == 0 && f) {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        char ts[32] = {};
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        std::fprintf(f, "[%s] votv-coop standalone bootstrap loaded into PID %lu (no UE4SS)\n",
                     ts, ::GetCurrentProcessId());
        std::fclose(f);
    }
}

DWORD WINAPI BootThread(LPVOID) {
    WriteMarker();
    // Standalone SDK health check (resolves GUObjectArray / FName::ToString /
    // ProcessEvent via AOB, then functionally validates them). Logs a PASS/FAIL
    // report to votv-coop.log -- our own SDK access, no UE4SS.
    ue_wrap::log::Init();
    UE_LOGI("==== %s ====", coop::version::kDisplayLabel);
    UE_LOGI("boot: version-full=%s", coop::version::kVersionFull);
    ue_wrap::reflection::RunHealthCheck();

    // Establish a game-thread execution context: hook ProcessEvent so we have a
    // guaranteed game-thread callback to drive UFunction calls from (ProcessEvent
    // must NOT be called from this boot thread). Then post a self-test task to
    // prove it: the task runs on the game thread (a different thread than this
    // one) and reads engine state safely from there.
    const unsigned long bootTid = ::GetCurrentThreadId();
    UE_LOGI("boot: BootThread tid=%lu", bootTid);
    if (ue_wrap::game_thread::Install()) {
        ue_wrap::game_thread::Post([bootTid] {
            const unsigned long tid = ::GetCurrentThreadId();
            const int32_t n = ue_wrap::reflection::NumObjects();
            UE_LOGI("game-thread self-test: task ran on tid=%lu (boot tid=%lu, %s); "
                    "NumObjects()=%d read from game thread",
                    tid, bootTid, tid != bootTid ? "DIFFERENT thread -- OK" : "SAME -- WRONG",
                    n);
            UE_LOGI("==== GAME-THREAD CONTEXT: LIVE ====");
        });
        UE_LOGI("boot: game-thread dispatcher installed; self-test task posted");

        // Autonomous test harness (ported from the UE4SS Lua coopTestHarness):
        // skip the menus into gameplay, screenshot, report -- standalone.
        harness::Start();
    } else {
        UE_LOGE("boot: failed to install game-thread dispatcher");
    }
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        // Do real work off the loader lock.
        if (HANDLE t = ::CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr)) {
            ::CloseHandle(t);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Last-resort cleanup if WM_CLOSE never reached us (engine quit
        // via console / fatal-error path). DoShutdown is idempotent --
        // if our wndproc already ran it, this is a no-op. CRITICAL: do
        // NOT join any threads or post GT::Post lambdas here (we're
        // under the loader lock; that deadlocks). DoShutdown only sets
        // a flag + uninstalls our PE detour, both safe under the lock.
        // The detached worker threads observe g_shuttingDown and exit
        // on their own; we don't wait for them.
        coop::shutdown::DoShutdown();
    }
    return TRUE;
}
