// multivoid bootstrap entry.
//
// The mod DLL has ONE way into the process (UE4SS_ARC WP-2 commit 3 retired
// the standalone xinput-proxy lane whole, its filename lane-discriminator
// included): UE4SS LoadLibrary's it as Mods/Multivoid/dlls/main.dll at
// mod-SCAN time (for every mod found, enabled or not) and starts ENABLED mods
// later via the exported start_mod() (src/loader/cppmod_entry.cpp). Nothing
// boots from ATTACH -- a disabled mod folder is LOADED but never STARTED, so
// DllMain must not boot. DETACH keeps the last-resort teardown backstop.

#include "coop/session/shutdown.h"
#include "loader/cppmod_entry.h"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
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
