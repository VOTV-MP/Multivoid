// coop/world/spill_sync.h -- a spilled liquid's decal is the host's, and every peer has it where it landed.
//
// A coffee cup, beer, wine or a gas bottle spills through a grimeProjectile_C whose flight leaves a grime decal
// (ue_wrap/actors/grime_spill). Each peer would fly its own and land it at its own spot, so:
//   - HOST: a decal its projectile leaves is sent where it landed, with its decal's turn and its dirt (SpillState).
//   - CLIENT: a decal its own projectile leaves is removed. When that liquid was in this player's hand, the decal is
//     sent to the host first (SpillIntent); the host checks its class and its reach, makes it and sends it back. A
//     host's decal is made here at the same transform, unless one of that class already lies there.
// Made at the same transform, a spill has the same position key on both peers, so coop/interactables/grime_sync
// carries its cleaning as it does a saved decal's. A joiner gets every live spill again, and keeps one its save already
// holds at those floats. A spill that just landed carries its splash, which every peer whose own flight did not land
// it plays there. A beer's or wine's pour plays its sound beside the bottle on the peer that pours it, so the host
// sends each of its pours (SpillPour), a client sends one from a bottle in its hand (SpillPourIntent) for the host to
// play and send on, and every peer that did not pour it plays it there. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct SpillPayload;
struct SpillPourPayload;
}  // namespace coop::net

namespace coop::spill_sync {

void Install(coop::net::Session* session);

// The hook's install, the decals the hook saw, the inboxes.
void Tick();

// A client's receiver of the host's decal.
void OnState(const coop::net::SpillPayload& p);

// The host's receiver of a client's spill.
void OnIntent(int senderSlot, const coop::net::SpillPayload& p);

// Every live spill again, to a joiner.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

// A client's receiver of a pour the host heard or relays.
void OnPour(const coop::net::SpillPourPayload& p);

// The host's receiver of a pour from a bottle in a client's hand.
void OnPourIntent(int senderSlot, const coop::net::SpillPourPayload& p);

// How many splashes this peer played for spills another peer's flight landed, how many pours of other peers' bottles
// it played, and how many pours it sent. Read by the spill drill.
uint32_t SplashesPlayed();
uint32_t PoursHeard();
uint32_t PoursSent();

}  // namespace coop::spill_sync
