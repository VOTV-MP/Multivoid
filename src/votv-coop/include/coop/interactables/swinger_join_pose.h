// coop/interactables/swinger_join_pose.h -- where an open swinger lid rests, for a joiner.
//
// The game saves nothing for a swinger (a child actor is skipped by the save), and the container lane's connect
// snapshot carries only open or closed, so a joiner's lid spawns at its hinge default. A closed lid is locked
// there on every peer and needs nothing. An open one rests wherever it was left, which only the host's live copy
// knows (a released lid's coast puts it where the releaser's stopped). So the host sends, right after the
// container lane's snapshot, each open lid's place under the lane's portable key (SwingerRestPose), and the joiner
// puts its copy there once the key resolves and its copy reads open. MTA's shape: the server's entity snapshot at
// join carries each element's live position, not its spawn one.
//
// A joiner mid-drag or mid-coast gets the snapshot, then the live pose stream, then the release. A lid a remote
// stream drives, or this peer's own player drags, is left to that owner.
//
// Game thread throughout. Wired from interactable_sync (the connect snapshot, the tick, the disconnect).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::swinger_join_pose {

// True when the joiner send covers the lid under `laneKey`: a portable `mv_` key that fits the wire key.
bool SendsRestPose(const std::wstring& laneKey);

// HOST: send `peerSlot` the place of every open lid the container lane indexes. A lid without a portable key is
// skipped (the receiver finds a lid by that key alone); one line per call says how many went and were skipped.
void QueueConnectBroadcastForSlot(coop::net::Session* session, int peerSlot);

// CLIENT: a rest pose arrived (validated at the dispatch edge). Applied now when the lid resolves and reads open,
// else parked for the tick.
void OnReliable(const coop::net::SwingerRestPosePayload& p);

// A remote drive started on the lid under `key`, or a release for it landed: the live stream and the release are
// newer than the connect snapshot, so a pose still parked for it is dropped. No-op when none is parked.
void Forget(const std::wstring& key);

// Retries the parked poses once per container-index pass, and drops one by the lane's own rule for its parked
// states. Free when nothing is parked.
void Tick();

// Session end: forgets every parked pose.
void OnDisconnect();

namespace dev {   // called only from src/coop/dev/ (the swinger drill); compiled always, inert unless called
// How the last rest pose for `key` stands: None (none arrived), Pending (parked), Applied (the lid was placed) or
// Dropped (a stream or a local drag owned the lid, the wait ran out, or the pending cap refused the pose).
enum class State : uint8_t { None, Pending, Applied, Dropped };
State Outcome(const std::wstring& key);
}  // namespace dev

}  // namespace coop::swinger_join_pose
