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

// A slot's alive state changed. The first pose a slot sends after its puppet came up sets the state and raises nothing
// (there is no "from" to change).
using LifeFn = void (*)(uint8_t slot, bool alive);
// One fresh pose of a slot, after its life edge (if any) was raised.
using PoseFn = void (*)(uint8_t slot, const coop::net::PoseSnapshot& pose);

// Adds a listener once (the same function again is a no-op that answers true). False when the fixed list is full.
bool AddLifeListener(LifeFn fn);
bool AddPoseListener(PoseFn fn);

// The puppet drive calls it once per fresh pose of `slot`, after the pose reached the puppet: the life edge first, then
// the pose listeners.
void Announce(uint8_t slot, const coop::net::PoseSnapshot& pose);

// The slot's puppet is gone (a leave, a session end): its alive state is forgotten, so the next puppet's first pose is
// silent again.
void ForgetSlot(uint8_t slot);

}  // namespace coop::puppet_events
