// bootstrap/boot.h -- the ONE boot entry.
//
// The mod enters the process through UE4SS's C-ABI start_mod() on
// Mods/Multivoid/dlls/main.dll (src/loader/cppmod_entry.cpp), which funnels here. StartOnce
// owns the two cross-cutting guards:
//   - the per-MODULE latch: one boot attempt per module instance, ever. A second call is
//     UE4SS's "Restart All Mods" re-entry and must NOT re-bootstrap a live session.
//   - the per-PROCESS duplicate guard: a PID-suffixed named mutex, so two instances of the
//     mod in ONE process -- two mod-folder copies, or a mod folder beside an older standalone
//     install that the loader's predecessor scan missed -- collide here. Deliberately not
//     machine-wide (the Global namespace): several game processes on one box is an ordinary
//     local session, never a duplicate.

#pragma once

namespace bootstrap {

enum class StartResult {
    kStarted,          // this call won the latch and spawned BootThread
    kAlreadyBooted,    // benign re-entry: this module instance already booted
    kRefusedDupMutex,  // another instance of the mod already booted THIS process
    // CreateThread failed -- nothing is running and nothing ever will on this module
    // instance. The boot latch is already taken by then, so a re-entry reads this
    // instance as previously-refused (AlreadyBooted() && !Started()).
    kRefusedThreadSpawn,
};

// entryTag names the entry point in the log and timing markers; "cppmod" is the one
// live entry.
StartResult StartOnce(const char* entryTag);

// True once a boot was ATTEMPTED on THIS module instance, booted or refused. Lets a
// re-entry caller (the UE4SS restart) short-circuit BEFORE side-effectful checks.
bool AlreadyBooted();

// True only when the attempt actually STARTED the bootstrap (kStarted). A
// module that latched but was refused (duplicate-mutex) reads attempted-but-
// not-started -- its restart re-entry must stay refused, not claim a session.
bool Started();

}  // namespace bootstrap
