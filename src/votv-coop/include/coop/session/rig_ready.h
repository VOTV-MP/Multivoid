// coop/session/rig_ready.h -- the one log line a test rig waits on.
//
// A rig that waits a fixed number of seconds for a peer wastes the run when the peer is ready
// sooner and measures nothing when it is not, so each peer says when it has reached a milestone.
// Every other log line may be reworded; this one is a contract with tools outside the tree:
//   rig: READY <milestone>[ slot=<n>]
//   hosting            the host's session is up and its world has settled, once per session
//   world-ready        this client announced ClientWorldReady: its world is up, its registry is
//                      coherent and the load's tail has settled; the host's replay has NOT arrived
//   joined             this client's join is over: the host's snapshot is applied and the join
//                      cover is down. The line to wait on before driving a client
//   peer-world-ready   the host took a client's ClientWorldReady and is replaying to slot <n>
//   solo-world         a gameplay world with NO session, from the menu ([dev] menu_autoload)
// A milestone is said every time it is reached: a client says `world-ready` again after a world
// change, the host says `peer-world-ready` for every join and rejoin.

#pragma once

#include "coop/dev/dependency_dump.h"
#include "ue_wrap/core/log.h"
#include "ui/imgui_overlay.h"

namespace coop::rig_ready {

// Each milestone also reaches the dependency dump and the overlay, which judges its draw seams at the first
// of hosting, world-ready and solo-world.

inline void Say(const char* milestone) {
    UE_LOGI("rig: READY %s", milestone);
    coop::dev::dependency_dump::OnMilestone(milestone);
    ui::imgui_overlay::OnMilestone(milestone);
}
inline void Say(const char* milestone, int slot) {
    UE_LOGI("rig: READY %s slot=%d", milestone, slot);
    coop::dev::dependency_dump::OnMilestone(milestone);
    ui::imgui_overlay::OnMilestone(milestone);
}

}  // namespace coop::rig_ready
