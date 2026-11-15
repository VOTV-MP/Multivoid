// coop/world/signal_spawn_copy.h -- the base computer's signal spawns (the Tardis, the TriFO, the shitting duende) as the
// world-actor lane carries them.
//
// analogDScreenTest_C mints them on the host when a signal's download completes (coop/creatures/npc_world_enum names the
// source and its three outputs); each is a world-anchored actor, so a client holds a copy at the host's place that lasts as
// long as the host's. The copies run their own presentation (the Tardis's timeline and sounds, the TriFO's flight, spin and
// blinking lights, the duende's dissolve when the local camera sees it) and none of their gameplay (coop/world/spawn_authority).
//
// The Tardis is the one that carries a birth value. It sits invisible for `delay` seconds (the desk sets 60 on it between its
// begin and its finish; the class default is another value) and then fades in with its sound, so a copy born with the class
// default would appear at another time than the host's. The host sends the seconds still to run, and the client writes them into
// the copy's `delay` before its begin play reads it. The value rides the world-actor spawn's existing birth bytes. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::signal_spawn_copy {

// Whether `cls` (a UClass) is tardis_C, and whether a class name is.
bool IsTardisClass(void* cls);
bool IsTardisName(const wchar_t* className);

// HOST: the birth bytes for a world-actor spawn of `actor`: for a Tardis, one float, the seconds before it appears (0 once it
// has), counted from the first time this actor was asked about (its enrol, a tick after its spawn); for any other class, 0
// bytes. Writes into `out` (room for `cap` bytes) and returns the length. A Tardis whose `delay` cannot be read returns 0
// and says so: the copy is then born with the class default.
uint8_t FillBirth(void* cls, void* actor, uint8_t* out, size_t cap);

// CLIENT: writes the host's remaining delay into a Tardis copy before its finish. False when the value is not usable or the
// property does not resolve (said); the copy then runs on the class default.
bool SeedTardisCopy(void* actor, const uint8_t* birth, uint8_t birthLen);

void OnDisconnect();

}  // namespace coop::signal_spawn_copy
