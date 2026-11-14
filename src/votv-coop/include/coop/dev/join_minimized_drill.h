// coop/dev/join_minimized_drill.h -- [dev] a client whose game window never presents a frame completes its join.
//
// A window minimized or covered through its join has no ImGui context (the overlay comes up on the first
// presented frame), so any ImGui call the SnapshotComplete case makes on the game thread faults and skips the
// rest of the case: the reconcile window stays up all session. This drill reproduces that window. At boot the
// client minimizes its own game window; the join runs with no frame presented. The case is synchronous on the
// game thread, so the first tick after `rig: READY joined` already sees its effect:
//   PASS     the reconcile window is down at that tick (the rig also expects its `LOWERED (SnapshotComplete)` line).
//   FAIL     the window is still up: the case did not finish. Said at once, no later leg.
//   INVALID  a frame was presented before the join completed, so the minimize did not take.
// After a PASS the window is restored, and the drill waits for the first presented frame and for the curtain
// to finish its fade (join_curtain::IsActive false), then prints `join-minimized: DONE on client`.
// Off unless config join_minimized_drill is set; client only. Game thread, except OnMilestone.

#pragma once

namespace coop::dev::join_minimized_drill {

// Minimize the game window once it exists, hold it minimized through the join, judge at the first tick after
// the join completed, then run the restore leg. From harness::pump::TickFrameTail. Game thread.
void OnFrameTail();

// Every readiness milestone the rig is told (coop/session/rig_ready.h); `joined` marks the join complete.
// Any thread.
void OnMilestone(const char* milestone);

}  // namespace coop::dev::join_minimized_drill
