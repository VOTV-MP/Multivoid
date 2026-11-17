// coop/world/cave_rock_sync.h -- the Antibreather cave's entrance rock is the host's, and stays open while any player
// is near it.
//
// antibreatherSPawner_C keeps the cave shut with `blockRock`, a component shown with its collision on. Its `block`
// timer (every 5 s) hides it while its Antibreather lives, and shows it again once player 0 is past 50 m; its BeginPlay
// hides it for a player starting inside. Each copy ran it on its own player and its own spawn rolls, so the cave stood
// open for a client and shut for the host (Ghezra/Multivoid#39). HOST: a closing `block` is undone while any player is
// within 50 m of the spawner; the rock is read twice a second and a change sent (CaveRockState), every rock to a joiner.
// CLIENT: its own `block` is refused; it shows or hides the rock, collision with it, as the host's. Game thread.

#pragma once

namespace coop::net {
class Session;
struct CaveRockPayload;
}  // namespace coop::net

namespace coop::cave_rock_sync {

void Install(coop::net::Session* session);

// The host's reads; the watch retried until registered. Game thread, once per pump tick.
void Tick();

// A client's receiver.
void OnState(const coop::net::CaveRockPayload& p);

// HOST: every rock, to a joiner at its ready edge.
void SendJoin(int slot);

void OnDisconnect();

}  // namespace coop::cave_rock_sync
