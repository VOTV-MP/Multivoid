// coop/dev/native_spawn_probe.h -- [dev] the native-spawn probe of the NPC-spawner arc (NB): every allowlisted
// creature this machine spawns from a Blueprint graph, recorded per source. Ini native_spawn_probe=off|record; BOTH
// peers. Game thread; plain statics.
//
// The record is a post hook on GameplayStatics::BeginDeferredActorSpawnFromClass, silent while no session runs. A
// call is kept when its result descends from an NPC-allowlisted class and its calling frame's object is not the
// GameplayStatics default object (a ProcessEvent-dispatched call). The key is the source class, the spawned class,
// the calling function, its code offset (the END of the call expression) and, on a client, whether the ghost sweep
// had run; a new key prints once, a client's also as NATIVE, and every count prints at session end. A full key
// table prints `record FULL` once.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::native_spawn_probe {

// Install the record when the row is on; once it has, a call returns at the latch. Re-entered every pump tick.
void Install(coop::net::Session* session);

// Session end: the per-key counts are printed and the records cleared.
void OnDisconnect();

}  // namespace coop::dev::native_spawn_probe
