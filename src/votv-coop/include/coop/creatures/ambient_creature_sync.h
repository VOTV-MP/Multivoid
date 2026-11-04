// coop/creatures/ambient_creature_sync.h -- one set of deer and walking trees: the host's.
//
// Both spawners roll absolute map points, so the host rolls and the npc lane mirrors what it rolled; a client's spawner has
// its tick taken off, as a new refusal row would need the maintainer's fingerprint table, and a creature that escaped before
// that goes. A deer dissolves when the viewing camera comes near, and each peer judges that for itself: a mirror's actor tick
// stays off, because its body turns the mesh by a floor a parked mirror does not have, so this lane runs the test. A tree's
// random size and its landing do not reach a mirror through the pose stream, so they cross here. This poll also takes the
// tick from the hexahive's and sus hole's spawners (coop/creatures/ambient_fixture_sync).

#pragma once

namespace coop::net {
class Session;
struct AmbientTreeStatePayload;
}  // namespace coop::net

namespace coop::ambient_creature_sync {

void Install(coop::net::Session* session);

// The poll, on either role.
void Tick();

// A client's receiver: the host's tree.
void OnTreeState(const coop::net::AmbientTreeStatePayload& p);

// Every tree again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// A client's spawners tick again, for the world it stays in.
void OnDisconnect();

}  // namespace coop::ambient_creature_sync
