// coop/creatures/ambient_creature_sync.h -- one set of deer and walking trees: the host's.
//
// Both spawners roll absolute map points, so the host rolls and the npc lane mirrors what it rolled; a client's two spawner
// ticks are refused at the gate (coop/world/spawn_authority). A tree's random size and its landing do not reach a mirror
// through the pose stream, so they cross here, on their edges: the host reads a tree when the engine announces its birth
// (coop/world/ambient_births) and again at the entry of its landing (a watch on walkingTree_C's OnLanded), and sends only
// what changed, as a joiner's resend. A client writes the size onto the mirror of that tree, again for a mirror born after
// the message. A deer dissolves when the viewing camera comes near, and each peer judges that for itself: a mirror's actor
// tick stays off, because its body turns the mesh by a floor a parked mirror does not have, so this lane runs the test.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientTreeStatePayload;
}  // namespace coop::net

namespace coop::ambient_creature_sync {

// The session the lane sends on, and the landing watch (retried until the gate takes it).
void Install(coop::net::Session* session);

// Drain the trees' births and deaths, then send (host) or apply (client); a client also runs the deer's dissolve.
void Tick();

// A client's receiver: the host's tree.
void OnTreeState(const coop::net::AmbientTreeStatePayload& p);

// Every tree again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// Read-only, for the drill: this session's totals.
struct Counters {
    uint64_t sent = 0;     // HOST: tree states sent
    uint64_t applied = 0;  // CLIENT: tree states written onto a mirror
};
Counters Read();

// The lane's state ends with the session.
void OnDisconnect();

}  // namespace coop::ambient_creature_sync
