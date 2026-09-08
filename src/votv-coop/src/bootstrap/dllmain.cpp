// multivoid bootstrap entry.
//
// The mod DLL has ONE way into the process: UE4SS LoadLibrary's it as Mods/Multivoid/dlls/main.dll
// at mod-SCAN time (for every mod found, enabled or not) and starts ENABLED mods later through the
// exported start_mod() (src/loader/cppmod_entry.cpp). Nothing boots from ATTACH -- a disabled mod
// folder is LOADED but never STARTED, so DllMain must not boot. DETACH keeps the last-resort
// teardown backstop.

#include "ue_wrap/core/gc_pin.h"
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
        // FIRST, and it is one relaxed atomic store -- nothing else. From here a GcPin releases
        // WITHOUT touching the engine or the registry lock.
        //
        // It has to be HERE and not only in DoShutdown, because DoShutdown runs only from
        // CoopWndProc's close branch: the game's own quit (RequestExit -> FEngineLoop::Exit) never
        // reaches it. Pins live inside statics -- the proxy map holds up to ~871 -- and the CRT
        // runs those destructors after this callback, where un-rooting would deref a freed UObject
        // and walk a GUObjectArray UE has already torn down.
        ue_wrap::GcPin::StopReleases();
        // PERSIST ONLY. The module is PINNED at start_mod (GET_MODULE_HANDLE_EX_FLAG_PIN in
        // cppmod_entry.cpp), so FreeLibrary cannot unload us and this branch is ALWAYS process
        // exit, where every other thread is already dead. Calling the full shutdown from here would
        // take the loader lock through Session::Stop's thread join and ~200 ms linger pump, the
        // signaling client's WSACleanup, and two MinHook uninstall passes that freeze threads --
        // the deadlock risk ue_wrap/core/hook.cpp already carries -- to quiesce what has already
        // stopped. The durable write is the part that still matters at process exit, so it is the
        // only part left.
        coop::shutdown::PersistAtProcessExit();
    }
    return TRUE;
}
