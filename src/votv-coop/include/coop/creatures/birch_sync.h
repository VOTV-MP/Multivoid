// coop/creatures/birch_sync.h -- the Killer Birch is the host's, and its kill reaches the player it caught.
//
// birchSpawner_C mints a birch_C when its player walks into its area, each machine for its own player, so each player
// had their own birch and the host's reached no one. HOST: the spawner's birch crosses as a world-actor mirror (the
// spawner a catch source; the mirror stamped as the spawner stamps it), and a client's spawner tick is refused. The
// birch's kill casts whoever stands in it to the player class, so a client's body in the host's birch ran the kill on
// the host: the punch, a black screen, the host's own teleport and save, then the menu. A punch on a body is
// delivered to that client as the game's (coop/player/player_damage, the birch a delivered caller); the rest of that
// kill is refused on the host: the teleport and the save a birch asks, its `sav` (the menu), and its black screen,
// unless the birch punched the host's own player. A client's swing at the birch is a hit intent. Game thread.

#pragma once

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::birch_sync {

inline constexpr const wchar_t* kBirchClass = L"birch_C";

void Install(coop::net::Session* session);

// The watches, retried until registered; the host's black-screen sweep after a body's kill. Game thread, once a tick.
void Tick();

void OnDisconnect();

}  // namespace coop::birch_sync
