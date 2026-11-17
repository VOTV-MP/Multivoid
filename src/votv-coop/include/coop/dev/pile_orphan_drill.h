// coop/dev/pile_orphan_drill.h -- [dev] the pile orphan drill: a chip pile and a garbage clump the host loses AFTER a
// joiner's world was cut must be gone from the joiner once the join sweep has run (ini pile_orphan_drill=1 / env
// VOTVCOOP_PILE_ORPHAN_DRILL, BOTH peers, with VOTVCOOP_CAN_DIR set to one folder for both; run it on a fresh host
// world, `mp.py smoke --host-fresh`).
//
// The joiner's world is a live capture of the host's, taken when the joiner asks for it, so a pile the host lost
// BEFORE that instant is not in the blob and is no orphan. The HOST therefore acts right after the capture
// (save_transfer): it destroys one isolated chip pile and the clump through the game's own destroy call, and leaves
// the signal `pile-orphan-staged` carrying their class and position (log line `pile-orphan: STAGED`). The CLIENT
// reads it when its sweep begins (INVALID if the signal is missing or it holds no native at either spot, so the
// check cannot pass for want of an orphan) and, at the sweep's done line, looks for a native of that class within
// 1 cm of each spot: `rig: VERDICT pile-orphan FAIL` the moment either is alive, PASS when both are gone.
// The reds are the two sweep rows sweep_bound_count_off (the floor keeps the pile) and sweep_fresh_universe_off
// (nothing finds either). Game thread.

#pragma once

namespace coop::dev::pile_orphan_drill {

// Once at harness boot; a latched bool read when off. Starts the peer signal's clock there, since the helper takes only a
// signal written after it: the host writes when the joiner's world is cut, long before the joiner has a pawn, and no pump
// (the install pump included) runs on a client before its world is up.
void Init();

// The session ended: a rejoin's client reads and judges again.
void OnDisconnect();

// HOST, save_transfer: the capture of slot `peerSlot`'s world has just been taken. Stages the orphans.
void NoteCaptured(int peerSlot);

// CLIENT, join_membership_sweep: the sweep is about to adjudicate. Reads the staging signal and checks the natives exist.
void NoteSweepBegin();

// CLIENT, join_membership_sweep: the lost-bracket backstop declared quiescence and no sweep ran. Says INVALID.
void NoteNoSweep();

// CLIENT, join_membership_sweep: the sweep has run (`aborted`: the >50% valve stopped it). Judges.
void NoteSweepDone(bool aborted);

}  // namespace coop::dev::pile_orphan_drill
