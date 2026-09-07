// harness/mod_environment.h -- census the OTHER mods sharing this process, and log what is present.
//
// It exists because a player reported 60 fps on a 120 fps machine, and a whole session went into
// instrumenting Multivoid for a cost that was never ours. A controlled measurement has since named
// the cause, and it is not the other mods: on one save, one build and one install, swapping only
// the UE4SS build moved the dev rig from ~70 to ~118 fps, while disabling the one Lua mod that
// build fails to start moved it ~5. The loader is worth about ten times its own mod set, and the
// installer now lays down the fast build -- so this module raises NOTHING to the player and only
// logs. What it can name is which foreign mods are present, which is what a bug report needs.
//
// Detection is pure file-system reading next to the exe -- no engine calls, no reflection -- so it
// is safe from the boot thread before any world exists. It must work on BOTH install lanes: a hand
// install keeps foreign Blueprint paks at the top of LogicMods, while shimloader gives every
// package its own LogicMods\<Team>-<Name>\ subdirectory, and scanning only one of those was a real
// defect.

#pragma once

namespace harness::mod_environment {

// Census the loader environment and LOG it. Idempotent; safe to call once at boot. It raises
// NOTHING to the player -- see the .cpp for why. One directory read, once, at boot.
void Run();

}  // namespace harness::mod_environment
