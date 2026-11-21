// coop/dev/kdress_drill.h -- drill: an Omega's outfit and its pat (ini kdress_drill=off|puton|takeoff|pat|born|join / env
// VOTVCOOP_KDRESS_DRILL; BOTH peers; the reds are the values of kdress_red: apply, ask, pat, snapshot).
// Each arm ends on one `rig: VERDICT kdress PASS|FAIL|INVALID` line, said by the host (coop/session/rig_verdict.h); a FAIL is
// said where it is decided and nothing after it runs, a bound only says INVALID. The host spawns the targets near the client's
// puppet (no walk, no teleport); the peers pace each other with drill_chat lines.
//   puton    the client holds the glasses and uses them on an Omega as a player does; the client's copy wears them, its hand
//            is empty, and the host's copy wears them once the host took the commit.
//   takeoff  the client takes glasses the host put on off through upgradeTake; both copies lack them and one glasses item
//            stands beside the Omega on both.
//   pat      the client pats an Omega and the host's copy plays the montage; the host pats it and the client's copy plays it.
//   born     the host spawns an Omega and dresses it in the same tick (the spawn has registered it by then); the client's copy
//            wears the row at its bind, told by the updateDrip send. No red reads it.
//   join     the host dresses an Omega before the client is launched, takes the row off after the joiner's capture, before its
//            world-ready; the joiner's copy lacks it after the host's join send. join says INVALID when its red cannot fail.

#pragma once

namespace coop::net { class Session; }

namespace coop::dev::kdress_drill {

// Advance this peer's steps. Every pump tick in a world. Game thread. A latched enum read when off.
void Tick(coop::net::Session* session);

// The session ended: back to the first step.
void OnDisconnect();

}  // namespace coop::dev::kdress_drill
