// coop/dev/join_minimized_drill.h -- [dev] a client whose game window never presents a frame completes its join.
//
// A window that never presented has no ImGui context, so an ImGui call made by the SnapshotComplete case on
// the game thread faults and skips the rest of it: the reconcile window stays up. At boot the client minimizes
// its own window, so the join runs with no frame. The first tick after `rig: READY joined` judges:
//   FAIL     the reconcile window is still up. Said at once, no later leg.
// Lowered by the snapshot's own completion, it restores the window and waits on the cover:
//   FAIL     the first presented frame left the cover up or waiting to fade.
//   PASS     the cover is Hidden after the window presented, then `join-minimized: DONE on client`.
//   INVALID  a frame came before the join completed, another cause lowered the window, no cover was up, or the
//            fade ran past a frame bound.
// Off unless config join_minimized_drill is set; on a host it logs that and does nothing.

#pragma once

namespace coop::dev::join_minimized_drill {

// Minimize the game window once it exists, hold it minimized through the join, judge at the first tick after
// the join completed, then run the restore leg. From harness::pump::TickFrameTail. Game thread.
void OnFrameTail();

// Every readiness milestone the rig is told (coop/session/rig_ready.h); `joined` marks the join complete.
// Any thread.
void OnMilestone(const char* milestone);

}  // namespace coop::dev::join_minimized_drill
