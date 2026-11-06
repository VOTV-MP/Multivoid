// coop/creatures/ambient_creature_sync.h -- one set of deer, walking trees and walking mannequins: the host's.
//
// Both spawners roll absolute map points, so the host rolls and the npc lane mirrors what it rolled; a client's two spawner
// ticks are refused at the gate (coop/world/spawn_authority). A tree's random size and its landing do not reach a mirror
// through the pose stream, so they cross here, on their edges: the host reads a tree when the engine announces its birth
// (coop/world/ambient_births) and again at the entry of its landing (a watch on walkingTree_C's OnLanded), and sends it when it
// changed, and again to a joiner alone. A client writes the size onto the mirror of that tree, again for a mirror born after
// the message. A deer dissolves when the viewing camera comes near, and each peer judges that for itself: its mirror keeps
// its actor tick (coop/creatures/npc_mirror), so the deer's own body runs the test, and the pose drive keeps the floor it reads.
// A walking mannequin is a prop and crosses as one (coop/creatures/walker_target, walker_brain, walker_state_*): the host's
// drive stream carries its walking, and a client's copy runs no brain. A walker freezes for any viewer: a client runs the
// walker's own view test on its copy and tells the host, which holds the walker's saw flag while any client has it on screen.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct AmbientTreeStatePayload;
struct WalkerWatchPayload;
}  // namespace coop::net

namespace coop::ambient_creature_sync {

// The session the lane sends on, and the landing watch (retried until the gate takes it).
void Install(coop::net::Session* session);

// Drain the trees' births and deaths, then send (host) or apply (client).
void Tick();

// A client's receiver: the host's tree.
void OnTreeState(const coop::net::AmbientTreeStatePayload& p);

// The host's receiver: a client's view of a walker.
void OnWalkerWatch(const coop::net::WalkerWatchPayload& p, int senderSlot);

// Every tree again, to that joiner's slot alone.
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
