// multivoid bootstrap entry -- the LANE DISCRIMINATOR.
//
// The mod DLL is loaded two ways (D-3 SLIM CONTRACT, spike 2026-08-21):
//   - the standalone xinput proxy (src/loader/xinput_proxy.cpp) LoadLibrary's
//     it under its versioned name multivoid-<game>-<build>.dll and expects it
//     to boot itself -- the shipping path today, dying whole at WP-2;
//   - UE4SS LoadLibrary's it as Mods/Multivoid/dlls/main.dll at mod-SCAN time
//     (for every mod found, enabled or not) and starts ENABLED mods later via
//     the exported start_mod() (src/loader/cppmod_entry.cpp).
// The module's OWN FILENAME is the honest discriminator between the two: the
// proxy/inject lane always maps us as multivoid-*.dll (that is the pattern it
// scans), the UE4SS lane always as main.dll. Booting from DllMain iff the
// proxy-era name matches keeps every existing flow (old proxy + new payload
// included) while honoring UE4SS enablement -- a disabled mod folder is
// LOADED but never STARTED, so it must not boot from DllMain.

#include "bootstrap/boot.h"
#include "coop/session/shutdown.h"
#include "loader/cppmod_entry.h"

#include <windows.h>

namespace {

bool OwnNameIsProxyEra(HMODULE self) {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') base = p + 1;
    }
    const size_t len = ::wcslen(base);
    // "multivoid-*.dll", case-insensitive (NTFS is case-preserving).
    return len > 14 && _wcsnicmp(base, L"multivoid-", 10) == 0 &&
           _wcsicmp(base + len - 4, L".dll") == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        // Proxy/inject lane boots here; the UE4SS lane (main.dll) waits for
        // start_mod(). StartOnce does real work off the loader lock (it only
        // latches + CreateThreads).
        if (OwnNameIsProxyEra(module)) {
            bootstrap::StartOnce("proxy-dllmain");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Final vtable-dispatch tally (one log line; no-op when the cppmod
        // lane never ran). Before DoShutdown so the line lands even if the
        // logger is torn down there someday.
        loader::cppmod::FinalDump();
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
