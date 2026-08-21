// loader/cppmod_entry.h -- the UE4SS C-ABI lane's cross-TU surface.
//
// The exports themselves (start_mod / uninstall_mod) are GetProcAddress'd by
// UE4SS and never called from our own code; only the census dump crosses TUs
// (dllmain's DETACH backstop prints the final tally).

#pragma once

namespace loader::cppmod {

// One log line with every nonzero vtable-slot counter (no-op when the cppmod
// lane never ran). Safe under the loader lock: log-write + flush only.
void FinalDump();

}  // namespace loader::cppmod
