// coop/dev/join_abort_drill.h -- [dev] the proof of the client session-end seam.
// A client presses the loading screen's Cancel mid-join (join_progress::RequestCancel), the
// abort routes through harness::session_runtime::EndClientSession, and the drill judges that the
// session's end tore the coop state down:
//   - arm 'download' (window A, before the pawn): no per-player apply blob survives the abort, so an
//     aborted join's inventory cannot bleed into the next join. Run with the host's
//     stall_world_stream_at_pct so the transfer cannot finish under the press. No rejoin leg.
//   - arm 'world' (window B, after this client announced world-ready): the world-ready announce state
//     is cleared after the stop, the player leaves the host's world for the menu, where the browser
//     opens, then the drill rejoins and passes at the second world-ready. Run with the host's
//     hold_snapshot_sec so the announced-world stretch stays open.
#pragma once

// The press rides TickFrameTail (which runs in BOTH the menu-mode transfer wait, via PostMenuTick,
// and the play loop, so it covers either window whichever loop is active) -- OnFrameTail. The judge
// and the rejoin dial ride the play loop's no-session branch -- OnNoSession -- which runs only after
// the stop site returned, so after EndClientSession's synchronous teardown, on the seam's bytes. Both
// run on the game thread; the drill's state is its own, outside dev_lanes::EndSession, so a rejoin does
// not reset it. Client only, off unless config join_abort is download|world.
//
// The drill uses no symbol the seam or the return to the menu adds, so it runs RED on the commit before
// each (the apply blob survives, the announce stays stale; the world stays with nothing leaving it) and
// GREEN after.

namespace coop::dev::join_abort_drill {

// Press Cancel at the arm's moment, and (world arm) note the second join's world-ready as the rejoin
// pass. From harness::pump::TickFrameTail -- the transfer wait and the play loop alike. Game thread.
void OnFrameTail();

// Judge the abort, and (world arm) where the player ended, then dial the rejoin. From the play loop's
// no-session branch, after the stop. `leaving`: whether a leave of this world for the menu was under
// way when the play loop posted this pass (harness::join_leave::Active, read on the thread that owns
// it). Game thread.
void OnNoSession(bool leaving);

}  // namespace coop::dev::join_abort_drill
