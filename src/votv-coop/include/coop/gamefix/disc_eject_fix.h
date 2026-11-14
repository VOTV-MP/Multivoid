// coop/gamefix/disc_eject_fix.h -- a disc ejected from the PC keeps what the PC's file screen did
// to it (game 0.10.0).
//
// The PC holds an inserted disc two ways: a byte snapshot of the disc actor, taken at the insert,
// and working copies of its rows and remaining writes, which the file screen edits. The eject
// spawns a disc and restores the snapshot. 0.9.0n then wrote the working rows and the remaining
// writes onto the new disc; 0.10.0's laptop_C does not (its serverBox_C still does), so a disc
// leaves the PC as it entered: rows moved off a floppy are back on it, rows moved onto a zip drive
// are gone, and no write is ever spent. This lane writes both, as 0.9.0n did: the rows and the
// count are read as the eject begins, before the game clears them, and written onto the disc the
// eject's end spawns, once the snapshot is restored. It runs on the machine whose eject it is,
// while a session holds the script gate, and only on the function body it was written for
// (coop/gamefix/body_guard.h).

#pragma once

#include <cstdint>

namespace coop::gamefix::disc_eject_fix {

// Register the watches. Idempotent. Any thread.
void Install();

// Off leaves the game's own eject alone: a drill's control arm. On by default.
void SetEnabled(bool on);

// How many ejected discs took their rows from this lane.
uint64_t Applied();

}  // namespace coop::gamefix::disc_eject_fix
