// coop/world/ambient_fixture_sync.h -- one set of dirt holes and beehive branches: the host's.
//
// A hexahive is a field of dirt holes, and its spawner destroys itself in its begin play, so the holes are what crosses. A lone
// sus hole is the same class. A beehive's branch is a saved actor that stands in the world; its nest is a prop and crosses on
// the prop lane. Neither moves, so the host's poll sends each one's place once, and its end. A client's hexahive and beehive
// spawners are refused at the gate (coop/world/spawn_authority), so it rolls neither; its sus hole spawner is not refused, and a
// hole it rolls stays its own. A client keeps the branches its load brought, as the host's save made them.
// A sus hole's spawner rolls around player 0's camera, which on the host is the host's, so the host makes the same roll around
// each client's body on the spawner's own clock, and the hole crosses as any other. Its end does not: a hole ends by its own
// viewer's distance and view, so the host's copy of a client's hole ends by the host and each copy elsewhere by its own player.

#pragma once

namespace coop::net {
class Session;
struct AmbientFixtureStatePayload;
}  // namespace coop::net

namespace coop::ambient_fixture_sync {

void Install(coop::net::Session* session);

// The poll, on either role.
void Tick();

// A client's receiver: the host's fixture, or its end.
void OnState(const coop::net::AmbientFixtureStatePayload& p);

// Every fixture again for a joiner; a client that has one already keeps it.
void QueueConnectBroadcastForSlot(int slot);

// A client's copies go, for the world it stays in.
void OnDisconnect();

// [dev], for the drill alone. HOST: one sus hole roll around client `slot`'s body now; true when it made a hole. Game thread.
bool DebugRollHoleNear(int slot);

}  // namespace coop::ambient_fixture_sync
