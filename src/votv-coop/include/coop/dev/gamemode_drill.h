// coop/dev/gamemode_drill.h -- [dev] the game mode's timer events, as steps of the event-fire drill (coop/dev/event_fire_drill),
// for the host-owned UFOs, the body, the mushroom duende and the Midas UFO, and the per-player Shadow Figura.
//
// Each step calls the game's own timer handler on mainGamemode_C by reflection, with the day clock held for the call at an hour
// and day its gate wants (restored before it returns, so no clock sample is sent), until what it mints stands. No player is moved;
// every 5 s each peer logs in `[GM-DRILL]` lines what it holds and where a flyer is. A client rolls the same handler at 30 s, which
// the spawn authority refuses; for the per-player Figura the client's own roll is the point.
//
//   gmUfoBall / gmUfoBoo / gmUfoPill   ufo_ball (ufo_ballfo_C), ufo_boo (ufo_boofo_spawn1_C), ufo_pill (ufo_pillfo_C).
//   gmUfoBallHold                      gmUfoBall with the saucer's tick stopped 12 s on, for a client that joins while it stands.
//   gmUfoJoel                          ufo_joel needs a player atop the radio tower, so at 6 s the host stands a ufo_joel_C and
//                                      enrols it as the catch seam does (world_actor_sync::HostEnrollExSpawn).
//   gmUfoMidas                         ufo_midas (the level's midasUfoTest_C is switched on through its trigger).
//   gmBody / gmFunguy                  tickerBody (theBody_C), tickerFunguy (npc_funguy_C).
//   gmFigura                           CustomEvent_3 (figura_C), rolled by BOTH peers.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::gamemode_drill {

// Every name this file has steps for; none has a row in the event table.
bool IsGamemodeDrill(const std::string& name);

// The client's pass, every tick of a connected client while the drill is on.
void ClientStep(const std::string& name);

// The host's pass, `sinceFireMs` after the fire.
void HostStep(const std::string& name, uint64_t sinceFireMs);

void OnDisconnect();

}  // namespace coop::dev::gamemode_drill
