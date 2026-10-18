// coop/dev/native_spawn_probe.h -- [dev] the native-spawn probe of the NPC-spawner arc (NB): every allowlisted
// creature this machine spawns from a Blueprint graph, recorded per source, and (orb) whether a client spawns its
// own black orb. Ini native_spawn_probe=off|record|orb; BOTH peers. Game thread; plain statics.
//
// The record is a post hook on GameplayStatics::BeginDeferredActorSpawnFromClass, silent while no session runs. A
// call is kept when its result descends from an NPC-allowlisted class and its calling frame's object is not the
// GameplayStatics default object (a ProcessEvent-dispatched call). The key is the source class, the spawned class,
// the calling function, its code offset (the END of the call expression) and, on a client, whether the ghost sweep
// had run; a new key prints once, a client's also as NATIVE, and every count prints at session end. A full key
// table prints `record FULL` once.
//
// orb, on a CLIENT after the ghost sweep: every blackballSpawner_C gets laps=241 and angle=540 each Tick until the
// record has seen npc_orborb_C from it (each such orb is destroyed on that Tick and checked gone on the next:
// `orb NOT-CONTAINED` if not) or 60 s pass (no spawner class: `not-spawned instances=0`). The spawners return to
// laps 0 and their own angle and "orb DONE" ends the run; a broken instrument prints `orb BROKEN`, then DONE.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::native_spawn_probe {

// Install the record when the row is on; once it has, a call returns at the latch. Re-entered every pump tick.
void Install(coop::net::Session* session);

// The pump tick's entry: the orb drive. A latched read when the row is off.
void Tick(coop::net::Session* session);

// Session end: the per-key counts are printed and the records cleared; an orb drive mid-way is abandoned
// (the spawners restored, the held orbs destroyed).
void OnDisconnect();

}  // namespace coop::dev::native_spawn_probe
