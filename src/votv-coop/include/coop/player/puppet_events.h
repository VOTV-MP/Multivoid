// coop/player/puppet_events.h -- what a remote player's pose stream announces: a death or a revive, and each fresh
// pose. A listener only RECORDS (it runs inside the pose receive, in the puppet drive's loop); the module that wants a
// reaction takes it on its own tick.
//
// There is no death packet: a player's health rides every pose (PoseSnapshot::healthFrac) and a health of zero or less
// is the only value that quantises to 0, so the edge is read from the stream. MTA announces a death as a packet the
// server receives (CGame.cpp:2141 Packet_PlayerWasted); ours is derived, because the health already travels with each
// pose and a second message would carry the same fact. Divergence: that packet also names the killer, the weapon and
// the body part (CGame.cpp:2141-2185); the pose carries none of them, so the cause of a death is lost here and a
// listener that needs it reads the evidence on the stream (the burning bit). The stream is level-triggered: every pose
// repeats the health, so a dropped unreliable pose delays the edge and never loses it. Game thread throughout.

#pragma once

#include <cstdint>

namespace coop::net { struct PoseSnapshot; }

namespace coop::puppet_events {

// A slot's alive state changed. The first pose a slot sends after its puppet came up has no "from": an alive one sets the
// state and raises nothing, a dead one is raised as a death (a peer dead when its puppet appears reaches every listener).
using LifeFn = void (*)(uint8_t slot, bool alive);
// One fresh pose of a slot, after its life edge (if any) was raised.
using PoseFn = void (*)(uint8_t slot, const coop::net::PoseSnapshot& pose);

// Adds a listener once (the same function again is a no-op that answers true). False when the fixed list is full.
bool AddLifeListener(LifeFn fn);
bool AddPoseListener(PoseFn fn);

// The puppet drive calls it once per fresh pose of `slot`, after the pose reached the puppet: the life edge first, then
// the pose listeners. `worldReady` is whether the peer has a world to be alive or dead in (the host's view of a slot:
// Session::IsSlotWorldReady; a client's puppets spawn only after its own world-ready): a pose before it carries no life
// state, so its health is held as unknown, raises nothing and changes nothing; the first pose at or after it is the
// first one. The pose listeners still hear every pose.
void Announce(uint8_t slot, const coop::net::PoseSnapshot& pose, bool worldReady);

// The slot's puppet is gone (a leave, a session end): its alive state is forgotten, so the next puppet's first pose
// has no "from" again.
void ForgetSlot(uint8_t slot);

// What a slot's pose says of its life, and what that does: the state to hold, whether a life edge is raised, and the
// edge's value. Pure (no listener, no slot state), so its truth table is checked on pinned poses by RunSelftest.
enum class Seen : uint8_t { Nothing, Alive, Dead };
struct LifeStep {
    Seen now;
    bool raise;
    bool alive;
};
LifeStep StepLife(Seen was, uint8_t healthFrac, bool worldReady);

// The un-gated truth table of StepLife on pinned poses (coop/player/puppet_events_selftest.cpp), once per session start.
// Pure, any thread; true when every check passed.
bool RunSelftest();

}  // namespace coop::puppet_events
