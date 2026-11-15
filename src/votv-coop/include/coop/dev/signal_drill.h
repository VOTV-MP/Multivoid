// coop/dev/signal_drill.h -- [dev] the base computer's signal spawns and the Alien Cart and Alien Catapult pranks, as steps of
// the event-fire drill (coop/dev/event_fire_drill). No player is moved.
//
// A signal step stands the desk's finished download as the game does (the signal name set, detection at 100 %), calls the desk's
// own `autoSave` on the host, and restores both before the call returns. Every 5 s each peer logs in `[SIG-DRILL]` lines what it
// holds of the product (the TriFO's `ufo` component, the Tardis's `delay` and `appeared`). At 30 s the client calls the same
// `autoSave` (refused by the desk) and, for the TriFO, spawn_trifo (refused by the spawn authority); its count must not rise.
//   sigTardis    the "tardis" signal: tardis_C, with its delay.
//   sigTrifo     the "trifo" signal: ufo_trifo_C.
//   sigDuende    the "shitduende" signal: shitDuende_C at the base toilet.
// The prank steps ride the event table: `hillRoller` and `alienJump` fire the eventer's special event and, 6 s on, the host calls
// the spawner's own `spawn` (what its box overlap or sight line calls). `rockThrow` is fired alone: its box overlaps only the
// host's player, so no rock is thrown and the step reports what each peer holds.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::signal_drill {

// The three signal drills' names; they have no row in the event table.
bool IsSignalDrill(const std::string& name);

// The client's pass, every tick of a connected client while the drill is on (the signal drills and the three prank names).
void ClientStep(const std::string& name);

// The host's pass, `sinceFireMs` after the fire.
void HostStep(const std::string& name, uint64_t sinceFireMs);

void OnDisconnect();

}  // namespace coop::dev::signal_drill
