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
        // PERSIST ONLY. This used to call coop::shutdown::DoShutdown() under a
        // comment claiming it "only sets a flag + uninstalls our PE detour, both
        // safe under the lock" -- and BOTH halves of that were false. It reached
        // a thread join, a 200 ms network linger loop, a socket close with
        // WSACleanup, two sleeps and two MinHook thread-freezes (a documented
        // loader-lock deadlock risk that hook.cpp:229-235 already had on file).
        // The comment described the intent; nobody had re-read the body.
        //
        // `[V]` the module is PINNED at start_mod (cppmod_entry.cpp:318-325), so
        // FreeLibrary cannot unload us and this branch is ALWAYS process exit --
        // where every other thread is already dead, which makes all of that
        // quiescing work meaningless as well as dangerous. What still matters is
        // the durable write, so that is all we do.
        //
        // Found by an external source review of the public tree, 2026-08-30.
        coop::shutdown::PersistAtProcessExit();
    }
    return TRUE;
}
