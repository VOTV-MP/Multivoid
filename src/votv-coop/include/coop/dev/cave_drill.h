// coop/dev/cave_drill.h -- [dev] the cave creatures' steps of the event-fire drill (coop/dev/event_fire_drill).
//
// `caveWolfgang`: the host stands the cave spawner (antibreatherSPawner_C) beside its player, calls its own `cencipede` once and
// ends the spawner at once (its 10 s roll would otherwise make a creature of its own), which mints Wolfgang. A client stands a
// spawner of its own and calls its `cencipede` too, before the host's call: the refusal's lines are the proof that it mints none,
// and its 10 s roll is refused the same way.
// `caveEgg`: the host stands the cave's egg (wolfgangEggs_C) away from every player and calls its own `damageByPlayer`, which
// mints an aggressive antibreather. The client stands a spawner and rolls it as above.
// No player is moved. Each peer says how many of each creature it holds at 20 s and 40 s, so the two logs can be compared.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::cave_drill {

bool IsCaveDrill(const std::string& name);

// The client's pass, every tick of a connected client while the drill is on.
void ClientStep(const std::string& name);

// The host's pass, `sinceFireMs` after the fire.
void HostStep(const std::string& name, uint64_t sinceFireMs);

void OnDisconnect();

}  // namespace coop::dev::cave_drill
