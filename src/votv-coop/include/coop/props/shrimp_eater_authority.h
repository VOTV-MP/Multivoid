// coop/props/shrimp_eater_authority.h -- the Arirals eat a shrimp no player is near or watching.
//
// shrimpEaterMaster_C's `eat`, every 15 s, takes the shrimp nearest the last one it ate and, unless player 0 is within
// 100 m of that spot or the shrimp was rendered in the last 5 s, plays the eat sound, leaves an empty pack and destroys
// the shrimp. Every peer ran its own against its own player: a client's eat destroyed the shrimp for everyone through
// the prop destroy seam, and its empty pack stayed on that client. CLIENT: its `eat` is refused. HOST: its `eat` is
// also refused for a shrimp a client's player is within 100 m of, or had in view in the last 5 s, read twice a second
// from that player's streamed eye and aim; the host's player stays the game's own check. Game thread.

#pragma once

namespace coop::net { class Session; }

namespace coop::shrimp_eater_authority {

void Install(coop::net::Session* session);

// The watch, retried until registered, and the host's view reads. Game thread, once per pump tick.
void Tick();

void OnDisconnect();

}  // namespace coop::shrimp_eater_authority
