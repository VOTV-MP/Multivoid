// coop/world/ambient_fixture_sync.h -- one set of dirt holes and beehive branches: the host's.
//
// A hexahive is a field of dirt holes, and its spawner destroys itself in its begin play, so the holes are what crosses. A lone
// sus hole is the same class. A beehive's branch is a saved actor that stands in the world; its nest is a prop and crosses on
// the prop lane. Neither moves, so the host sends each one's place once, and its end, as the engine announces its birth and
// death (coop/world/ambient_births). A client's hexahive and beehive spawners are refused at the gate (coop/world/spawn_authority),
// so it rolls neither; its sus hole spawner is not refused, and a hole it rolls stays its own.
// A branch is one branch by the key its nest was born with: the host reads it at the branch's own edges (the watches on getBee
// and loadData) and keeps it after a player takes the nest, which clears the branch's own. A client binds the loaded branch
// whose saved key is that key and spawns a copy only when none is, then gives the branch its nest by the key (the game's own
// getBee, once the nest's mirror is in the prop key index). A joiner is sent every fixture and then a census-end, by which it
// ends each loaded branch nothing named. A branch's end on a client leaves the nest to the prop lane.
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

// Drain the holes' and branches' births and deaths, then send (host) or apply (client), on either role.
void Tick();

// A client's receiver: the host's fixture, its end, or the census-end.
void OnState(const coop::net::AmbientFixtureStatePayload& p);

// Every fixture again for a joiner, to that slot alone, then its census-end.
void QueueConnectBroadcastForSlot(int slot);

// A client's copies go, for the world it stays in.
void OnDisconnect();

// [dev], for the drill alone. HOST: one sus hole roll around client `slot`'s body now; true when it made a hole. Game thread.
bool DebugRollHoleNear(int slot);

}  // namespace coop::ambient_fixture_sync
