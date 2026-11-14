// coop/world/ufo_sync.h -- the gamemode's ambient UFOs are the host's, and every player sees the same ones.
//
// mainGamemode_C arms five self-repeating timers at begin play: ufo_ball, ufo_boo, ufo_pill and ufo_joel each roll and
// mint a UFO (ufo_ballfo_C, ufo_boofo_spawn1_C, ufo_pillfo_C, ufo_joel_C); ufo_midas runs the level-placed Midas UFO
// (midasufotest_C, keyed "midasufo"). A client refuses all five (coop/world/spawn_authority); the host's UFOs reach
// every client on the world-actor lane, whose client copy runs the UFO's own animation. Two of them roll their path at
// birth on each machine -- the ball its turn and two ends, the boo its flight spline's wobble -- so the host sends what
// its copy rolled (UfoEvent) and the client writes it over its own before the copy flies. The host relays the Midas
// take-off, and each client runs its own. A copy the host destroys first gives back the event its script would have
// ended on its own way out. Game thread.
#pragma once

#include "ue_wrap/core/types.h"  // FVector

#include <cstdint>
#include <vector>

namespace coop::net {
class Session;
struct UfoEventPayload;
}  // namespace coop::net

namespace coop::ufo_sync {

// Keep the session and register the host's watch on the Midas UFO's runTrigger. Idempotent.
void Install(coop::net::Session* session);

// CLIENT: apply the paths whose copy has arrived. Settles the watch. Per frame.
void Tick();

// HOST (coop/world/world_actor_sync): a UFO's world-actor spawn went out, from the EX drain that has the live, rolled
// actor. Its path follows at once on the same ordered lane, so a client's copy never waits on a scan. `eid` is the
// element id the spawn carried.
void OnHostSpawnSent(void* actor, uint32_t eid);

// HOST, at a joiner's world-ready edge: every UFO still flying is enrolled if it is not (a session end drains the host's
// elements), and its path goes to the joiner; the Midas run too while it lasts.
void SendJoinSnapshotForSlot(int slot);

// [dev] What the lane has done since the process began (monotonic, so a drill's delta across a session end never
// underflows), for the UFO drill: paths the host sent, complete paths a client received and applied to
// a copy, and Midas runs a client received.
struct Counters {
    uint32_t pathsSent = 0, pathsReceived = 0, pathsApplied = 0, midasRuns = 0;
};
Counters Read();

// [dev] A ball's or a boo's path as the lane reads it (a ball's turn and two ends, a boo's flight spline), for the
// drill to compare the host's with a client copy's. False for another class or an unreadable path.
bool DevReadPath(void* ufo, std::vector<ue_wrap::FVector>& out);

// CLIENT: the host's UfoEvent, its length checked by the dispatcher.
void OnEvent(const coop::net::UfoEventPayload& p);

// CLIENT (coop/world/world_actor_mirror): a world-actor copy was made, its tick parked. A UFO's copy flies again at
// once, or a ball's once its path is written.
void OnMirrorMaterialized(void* actor);

// CLIENT (coop/world/world_actor_mirror): the host destroyed a world-actor copy; called before the destroy.
void OnMirrorEnding(void* actor);

// Session end: the records are cleared.
void OnDisconnect();

}  // namespace coop::ufo_sync
