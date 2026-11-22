// coop/world/arir_tower_ship_sync.h -- the Ariral ship beside the radio tower's top is the host's, seen by every player.
//
// arirShip_tower_C is placed beside the tower's top. Its begin play loops every 5 s: a shown ship fades out (its fade's
// end leaves a ufoDreamer_C and ends it), and a hidden one fades in on a 10% roll from day 20, from 21:00 to 03:00, while
// player 0 stands in its box and has not just seen it; a radial force then pushes the props near it. Every peer rolled it
// for its own player, so a client's ship showed on that client alone and pushed its own copies of the props. HOST: its
// `appear` is sent as it changes the ship (EventCue 106, x 1 or 0), and to a joiner while it is up, and every 5 s it
// rolls the game's own odds for each client's player in the box who is not looking at it. CLIENT: its own `appear` is
// refused, and the host's runs on its copy. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::arir_tower_ship_sync {

// The EventCue id; the cue registry's ids are append-only.
inline constexpr uint32_t kCueTowerShip = 106;

void Install(coop::net::Session* session);

// The watch, retried until registered, and the host's rolls for the clients' players. Game thread, once per pump tick.
void Tick();

// HOST: the ship to a joiner at its ready edge, while it is up.
void SendJoin(int slot);

// A client's receiver: true when the cue was this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

void OnDisconnect();

}  // namespace coop::arir_tower_ship_sync
