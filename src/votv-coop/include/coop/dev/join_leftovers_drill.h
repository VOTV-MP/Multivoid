// coop/dev/join_leftovers_drill.h -- [dev] the actors a joiner is owed after the host's last client left, as steps of the event-fire
// drill (coop/dev/event_fire_drill).
//
// Each name fires one of the dispatcher's events on the host (the event's own name is in the table below) and, from 12 s on, stops the
// product's tick so that it outlasts its flight; the list's `@rejoin` then kills the client and starts it again, and the host's
// world-actor lane, which dropped its rows with the client, enrols the standing actor at the joiner's world-ready
// (coop/world/world_actor_rejoin). Both peers say in `[JL-DRILL]` lines every 2 s how many of the class they hold and, for each,
// whether it is a copy of the host's (`mirror 1`) or an actor of this peer's own (`mirror 0`), so the second life shows one actor, a copy
// or an adopted save actor, and no twin. The host does not fire again when the actor still stands from before the rejoin.
//
//   jlBorg     borgRozital: rozitBorg_C (the mothership; no save, no level piece).
//   jlSalt     salt: saltpile_C (a save actor the joiner's own load holds, adopted as the copy).
//   jlMorning  morningGay: morningUfo_C (the morning UFO).

#pragma once

#include <cstdint>
#include <string>

namespace coop::dev::join_leftovers_drill {

bool IsLeftoversDrill(const std::string& name);

// The event-table name the drill fires, or null when `name` is none of its names.
const char* EventOf(const std::string& name);

// Whether the host already holds the drill's class from before a rejoin, so the event is not fired a second time.
bool AlreadyHeld(const std::string& name);

void ClientStep(const std::string& name);
void HostStep(const std::string& name, uint64_t sinceFireMs);
void OnDisconnect();

}  // namespace coop::dev::join_leftovers_drill
