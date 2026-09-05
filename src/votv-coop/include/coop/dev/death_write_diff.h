// coop/dev/death_write_diff.h -- what did the death episode actually change? The mod vetoes
// the game's death travel, so every one-way write the death makes survives with no owner (the
// game never undoes them; the world was about to go); the revive's reconcile disposes of the
// ones a hand census found, and that census rots silently on a game update. This instrument
// makes the set measured instead of enumerated: snapshot the death-relevant object graph
// before the lethal hit, diff it after the revive, print the raw delta. It is not a fix (it
// reads, never writes; it says which cells moved, not what they should be, and the sub-area
// and the unpause are measured leaves where restoring would be the bug), not a control run
// (two instants of one run, since runs diverge on damage, position, inventory and time), not
// armed on the dead flag (the damage quadrants are written before it exists, so the harness
// snapshots before delivering the hit), and not the whole set from one run (several damage
// sites, a direct kill, a drowning branch: run the arms, then union). Run it with the
// reconcile off, or our own fix hides the writes it must re-find. The scope is every live
// user widget, the GameInstance, the gamemode and the local pawn with their Outer children,
// plus objects that appeared; each cell is a byte compare at the field's own offset.

#pragma once

// The noise floor. Hundreds of cells moved in a run where the death changed almost nothing,
// and two defects of this instrument produced most of that. A cross-run key is not an
// identity: cells are named object-dot-field, and pooled widgets carry a per-run instance
// suffix, so differencing two runs mostly differences their object names. A bool bitfield is
// one byte shared by many flags, so a byte-wise cell reports the same change once per flag
// packed into it. The cure for the first is to subtract within one run, against a key fine
// enough to tell the findings apart: outer, object and field, both names stripped of a
// trailing instance suffix; a coarser key collapses every widget asset, since a
// designer-default name is itself type-plus-digits and the strip eats it. The harness stands
// the player still and takes a snapshot-and-diff pair across a control stretch, which measures
// what the world churns on its own; whatever moves there cannot be attributed to the death,
// and what survives the subtraction is what the census must account for. The floor must cover
// at least as long as the window it grades: the dead window is longer than the alive window,
// so a cell with a period between them would be reported death-attributable forever; the
// control stretch is at least as long as the graded span, and the longest stretch sets it.


namespace coop::dev::death_write_diff {

// Capture the death-relevant object graph. Game thread only (reflection and raw property
// reads). Returns the number of object-and-field cells captured, or -1 if the scope could not
// be resolved. Replaces any previous snapshot.
int Snapshot();

// Diff the live graph against the last snapshot and log the raw delta: one line per changed
// cell, plus died and appeared objects and per-class count deltas; `label` names the arm.
// Game thread only. With `learnNoise` the changed keys are recorded as the noise floor instead
// of being treated as findings; otherwise any cell whose key is in the floor is suppressed
// and only counted. Call it with true across the control stretches, then with false across
// the death. The two modes return different quantities on purpose, each the number a caller
// would act on: in learn mode the cells that moved (the floor's raw size), otherwise the
// death-attributable count after suppression. -1 if there is no snapshot, or if the world
// changed since it was taken: the sessionless arm lets the travel run, and a diff whose
// objects belong to a destroyed world is refused rather than reported.
int DiffAndLog(const char* label, bool learnNoise = false);

// Drop the learned noise floor, so a second scenario in one process starts clean.
void ResetNoiseFloor();

// Free the snapshot's storage once the last diff of a run has been logged: the snapshot is
// tens of megabytes and would otherwise be retained for the life of the process, which
// matters because the drill this serves grades a memory balloon, and an instrument that
// inflates the number it is measured beside is measuring itself.
void Release();

}  // namespace coop::dev::death_write_diff
