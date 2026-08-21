// bootstrap/boot.h -- the ONE boot entry both loader lanes call.
//
// D-3 SLIM CONTRACT (votv-ue4ss-f2-migration-DESIGN-2026-08-21.md SS2): the mod
// has two ways into the process -- the standalone xinput proxy (DllMain of a
// multivoid-*.dll, dying at WP-2) and UE4SS's C-ABI start_mod() on
// Mods/Multivoid/dlls/main.dll (src/loader/cppmod_entry.cpp). Both funnel here.
// StartOnce owns the two cross-cutting guards:
//   - the per-MODULE latch (one boot attempt per module instance, ever; the
//     project's standard Install() latch shape -- a second call is the UE4SS
//     "Restart All Mods" re-entry and must NOT re-bootstrap a live session);
//   - the per-PROCESS duplicate guard (a PID-suffixed named mutex, taken by
//     WHICHEVER lane boots first, so two same-process instances of the mod --
//     two mod-folder copies, or folder-mod beside a standalone install that the
//     predecessor scan somehow missed -- collide here regardless of lane mix).
//     Deliberately NOT machine-wide ("Global\\"): 2..4 game processes on one
//     box is the project's own standard LAN workflow, never a duplicate.

#pragma once

namespace bootstrap {

enum class StartResult {
    kStarted,          // this call won the latch and spawned BootThread
    kAlreadyBooted,    // benign re-entry: this module instance already booted
    kRefusedDupMutex,  // another instance of the mod already booted THIS process
};

// entryTag names the lane for the log/timing markers: "proxy-dllmain" | "cppmod".
StartResult StartOnce(const char* entryTag);

// True once any lane ATTEMPTED the latch on THIS module instance (booted OR
// refused). Lets a re-entry caller (UE4SS restart) short-circuit BEFORE
// side-effectful checks.
bool AlreadyBooted();

// True only when the attempt actually STARTED the bootstrap (kStarted). A
// module that latched but was refused (duplicate-mutex) reads attempted-but-
// not-started -- its restart re-entry must stay refused, not claim a session.
bool Started();

}  // namespace bootstrap
