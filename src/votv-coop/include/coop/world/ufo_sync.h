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

#include <cstdint>

namespace coop::net {
class Session;
struct UfoEventPayload;
}  // namespace coop::net

namespace coop::ufo_sync {

// Keep the session and register the host's watch on the Midas UFO's runTrigger. Idempotent.
void Install(coop::net::Session* session);

// HOST: send each new UFO's path. CLIENT: apply the paths whose copy has arrived. Settles the watch. Per frame.
void Tick();

// HOST, at a joiner's world-ready edge: the path of every UFO still flying, and the Midas run while it lasts.
void SendJoinSnapshotForSlot(int slot);

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
