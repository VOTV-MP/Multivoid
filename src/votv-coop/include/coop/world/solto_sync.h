// coop/world/solto_sync.h -- the Soltomia craft's visit, as a client sees and hears it.
//
// The craft (soltomiaCleaning_C, the soltoClean event) is the host's: a client holds a world-actor copy whose begin
// play, box overlap and destroy are refused, and the ATV, the doors and the sponge reach it on their own lanes. What
// the craft's own graph shows does not: it stays hidden until a body enters its box, then squeaks, throws particles
// and talks, and when its door is opened it plays its leaving montage and ends. The host watches those two edges and
// sends one EventCue for each; a client shows its copy and starts the copy's own squeak, particles and talk loop on
// the first, and runs the copy's own `opened` on the second, with the sponge the montage would mint refused there
// (the host's sponge is a prop). A client that joins while the craft is at work is sent the first cue. Game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct EventCuePayload;
}  // namespace coop::net

namespace coop::solto_sync {

// The EventCue ids of the craft's start and of its leave; the cue registry's ids are append-only.
inline constexpr uint32_t kCueStart = 103;
inline constexpr uint32_t kCueLeave = 104;

// The host's two watches, registered once.
void Install(coop::net::Session* session);

// CLIENT: a cue whose copy was not born yet is tried again.
void Tick();

// CLIENT: the host's cue. False when the id is not this lane's.
bool OnCue(const coop::net::EventCuePayload& p);

// HOST, the join replay at a joiner's world-ready: the start cue to `slot` while a craft is at work.
void QueueConnectBroadcastForSlot(int slot);

void OnDisconnect();

}  // namespace coop::solto_sync
