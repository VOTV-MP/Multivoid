// coop/world/ambient_fixture_sync.h -- one set of dirt holes, beehive branches and picnic warp arrows: the host's.
//
// A hexahive is a field of dirt holes (its spawner destroys itself in its begin play), and a lone sus hole is the same class.
// A beehive's branch and the picnic's Ariral warp arrow (arirShipVanishing_C, spawned by its trigger_spawnProp_C on the host
// alone) are saved actors that stand in the world; a branch's nest is a prop and crosses on the prop lane. None moves, so the
// host sends each one's place once, and a branch's or an arrow's end, as the engine announces the birth and death
// (coop/world/ambient_births). A client's hexahive and beehive spawners are refused (coop/world/spawn_authority); the hole its
// own sus hole spawner rolls is sent to the host and made there (coop/world/sus_hole_intent), and crosses here as any other.
// A branch is one branch by the key its nest was born with, read at its own edges (getBee, loadData) and kept after a player
// takes the nest. A client binds the loaded branch of that key, or a loaded arrow at the host's place, spawns a copy only when
// none is, and gives a branch its nest by the key (the game's own getBee, once the nest's mirror is in the prop key index).
// A joiner is sent every fixture, the standing branches and arrows the host names at that join among them, then a census-end,
// by which it ends each loaded branch and arrow nothing named. A branch's end on a client leaves the nest to the prop lane. A
// hole's end does not cross, and an arrow fades: each by its own viewer's distance and view, on each peer for its own player.

#pragma once

#include <cstdint>

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

// Read-only, for the drill: this session's totals.
struct Counters {
    uint64_t sent = 0;          // HOST: fixture messages sent
    uint64_t censusSent = 0;    // HOST: census-ends sent
    uint64_t loaded = 0;        // CLIENT: keyed branches the load brought
    uint64_t bound = 0;         // CLIENT: loaded branches bound to the host's branch by key
    uint64_t holeCopies = 0;    // CLIENT: hole copies spawned
    uint64_t branchCopies = 0;  // CLIENT: branch copies spawned
    uint64_t shipCopies = 0;    // CLIENT: warp arrow copies spawned
    uint64_t shipsBound = 0;    // CLIENT: loaded warp arrows bound to the host's by place
    uint64_t nestsBound = 0;    // CLIENT: nests given to a branch
    uint64_t hiveDropped = 0;   // CLIENT: branches that ended holding a nest, which the branch's end let stand
    uint64_t hiveGone = 0;      // CLIENT: branches that ended after their nest had already ended by the prop lane
    uint64_t censusEnds = 0;    // CLIENT: census-ends received
    uint64_t censusEnded = 0;   // CLIENT: loaded branches those census-ends ended
};
Counters Read();

// A client's copies go, for the world it stays in.
void OnDisconnect();

}  // namespace coop::ambient_fixture_sync
