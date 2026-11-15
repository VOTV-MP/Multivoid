// coop/dev/world_event_drill.h -- [dev] steps of the event-fire drill (coop/dev/event_fire_drill) for coop/world/event_world_sync.
// Each calls the game's own function on the host, as its roll or edge does, moves no player, and logs `[WORLD-DRILL]` lines.
//   earthTp       fires the event: the sky's `tp`. Each peer reports its black-hole sky every 10 s to 130 s.
//   earthTpHold   the host puts the sky into the black hole by the game's two writes and holds it, for a late joiner.
//   arirShip      fires the row and stands the tower (arirShipAppear_C) where the save has none; the host runs its runTrigger at
//                 16 s and again at 50 s.
//   lakeGlow      the host calls the lake's setActive(true) at 10 s and (false) at 50 s, as the gamemode's lake roll does.
//   radioPoof     the host runs ticker_radioTowerPoof until a radiotowerPoof_C stands; the client's runs are refused.
//   redPhone      the host runs the panel's redPhoneEventTimer until the ring starts; the client runs its own too.
//   lockerHead    the host runs ticker_lockerHead until a locker holds a head; the client runs its own too.
//   lockerCorpse  the host stands a lockerCorpse_C beside its player: its gore is props and a decal.
//   fleshRain     the host stands an event_fleshRain_C at the origin; a client's own spawn of one is refused.
// Where the save lacks a level object (the tower, a locker), each peer stands its own at one fixed point.

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::world_event_drill {

// Every name this file has steps for, the table's two (earthTp, arirShip) included.
bool IsWorldEventDrill(const std::string& name);

// The names with no row of the event table: the caller fires nothing and looks no row up.
bool IsStandalone(const std::string& name);

// The client's pass, every tick of a connected client while the drill is on.
void ClientStep(const std::string& name);

// The host's pass, `sinceFireMs` after the fire.
void HostStep(const std::string& name, uint64_t sinceFireMs);

void OnDisconnect();

}  // namespace coop::dev::world_event_drill
