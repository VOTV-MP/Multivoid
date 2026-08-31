// coop/dev/death_write_diff.h -- what did the death episode actually CHANGE?
//
// THE QUESTION THIS ANSWERS, and why a list could not.
//
// We MinHook-detour `UGameplayStatics::OpenLevel` and VETO VOTV's death travel
// (docs/DEATH_ARC.md). The game never needs an undo for anything the death writes, because
// the world and the HUD are about to be destroyed -- so with the travel cancelled, every
// one-way write SURVIVES with no owner. `death_revive::ReconcileCancelledTravel()` disposes
// of the ones a HAND census found (DEATH_ARC 11.3b: 13 writes, 3 owed, 2 replayed).
//
// That census is correct for VOTV 0.9.0n and ROTS SILENTLY on a game update: a fourteenth
// one-way write would fail nothing, and the user would find it the way they found the last
// two -- BY LOOKING AT THE SCREEN. This module is the instrument that makes the set
// MEASURED instead of enumerated: snapshot the death-relevant object graph immediately
// before the lethal hit, diff it after the revive, and print the RAW delta.
//
// FOUR THINGS IT IS DELIBERATELY NOT.
//
//  1. NOT a fix and NOT shipped behaviour. It reads; it never writes. A diff tells you WHICH
//     cells moved -- it cannot tell you what they SHOULD be, nor whether they should be
//     restored at all (`gameInstance.subArea` and `SetGamePaused(false)` are both measured
//     LEAVES, where restoring the pre-death value would be the bug). The restore VALUE and
//     the restore-vs-leave POLICY stay human. Pretending otherwise would just migrate the
//     hand-made half from the field list to the policy beside it.
//
//  2. NOT a control RUN. The baseline is two INSTANTS OF ONE RUN. Two runs diverge on
//     damage, position, inventory and world time, so a cross-run diff measures the run, not
//     the death (`[[lesson-a-screenshot-and-a-log-line-are-two-instants]]` is the same
//     family: the last red-screen dig was derailed for hours by pairing a frame with a log
//     line from eleven seconds earlier).
//
//  3. NOT armed on `dead`. `Add Player Damage` writes the four damage quadrants BEFORE
//     `dead` exists, so a snapshot armed on the flag is already too late -- exactly
//     `[[lesson-a-chain-derived-design-is-blind-to-side-effects-before-the-chain]]`. The
//     drill CONTROLS the trigger, so the snapshot is taken immediately before the harness
//     delivers the hit and needs no production seam at all. (A production restorer would
//     have no such seam and would need a ROLLING snapshot -- a real cost asymmetry, recorded
//     here because it is the argument for keeping this an instrument.)
//
//  4. NOT "the set" from one run. There are seven `Add Player Damage` call sites plus a
//     direct `kill()`, and drowning has its own branch. One run yields ONE SAMPLE OF ONE
//     DEATH VARIANT; calling that a census is `[[lesson-a-hand-written-inventory-is-a-list-
//     not-a-census]]` one level up. Run the arms, then union them.
//
// AND IT MUST BE RUN WITH THE RECONCILE OFF. With `ReconcileCancelledTravel()` enabled our
// own fix hides the two writes the instrument most needs to re-find, which is
// `[[lesson-an-instrument-that-shares-the-defect-cancels-it]]`. `VOTVCOOP_DEATH_NO_RECONCILE=1`
// is the negative control and the RED arm; both arms are expected output, not a failure.
//
// SCOPE is measured, not guessed. The 13 known writes land on FIVE owners -- gamemode
// (saveSlot.health), a ui_UI child widget, the local pawn, `pause_mainMenu` (a SEPARATE
// top-level widget, NOT under ui_UI) and the GameInstance -- plus one CREATED widget
// (blackScreen_C). A `ui_UI`-only snapshot would have missed four of the thirteen, two of
// them among the three that were actually owed. So the scope is every live UUserWidget
// descendant (a class-chain census, not a name list), the GameInstance, the gamemode and the
// local pawn, each expanded over its Outer children so leaf UWidgets are reached -- and the
// diff also reports objects that APPEARED, because write #6 is a widget that did not exist
// at snapshot time.
//
// DEPTH costs nothing extra: a diff needs BYTES, not typed reads. Every cell is
// `memcmp` over `ElementSize * ArrayDim` at the field's own offset, walked with
// `EnumerateStructFields` + `SuperStructOf` -- the frozen-digest idiom
// (`[[lesson-refactor-equivalence-frozen-digest-instrument]]`). FString/TArray fields store
// POINTERS, so a realloc moves the bytes with no semantic change; that noise is REPORTED
// rather than filtered, because a filter written before the first reading is an allowlist
// guessing at its own answer.

#pragma once

// THE NOISE FLOOR, and why the first reading needed a second instrument.
//
// The first two runs worked -- both owed writes were re-found mechanically, by nobody naming
// them -- but 319 of ~1.22M cells moved in a run where the death changed almost nothing, and
// TWO defects of this instrument produced most of that:
//
//   (a) A CROSS-RUN key is not an identity. Cells are named `<object>.<field>`, and pooled
//       widgets carry a per-run instance suffix (`CanvasPanelSlot_2147457267`,
//       `DebugMod_..._LogElement_C_2147457198`, `ui_radarPoint_C_...`), so differencing two
//       RUNS mostly differences their object NAMES.
//       (`[[lesson-a-cooked-export-name-is-not-an-identity]]`, one layer up.)
//   (b) A bool BITFIELD is one byte shared by many flags, so a byte-wise cell reports the
//       same change once per flag that packs into it -- `bNetAddressable C7 -> 07` and
//       `bReplicates C7 -> 07` are the SAME byte. This is exactly the mis-attribution
//       `reflection.h`'s `FindBoolProperty` exists to prevent.
//
// The cure for (a) is to subtract WITHIN ONE RUN and to subtract by CLASS+FIELD rather than
// by instance name: the harness already stands the player still for a 10 s ALIVE control
// window before the hit, so a snapshot/diff pair across THAT window measures what this world
// churns on its own -- animated hints, radar points, pooled log rows, ticking floats. Any
// (class, field) that moves there cannot be attributed to the death. What survives the
// subtraction is death-attributable, and THAT is the set the census must account for.
//
// The control window is the same 10 s, the same cadence and the same frame load as the dead
// window by construction (the harness comment on `kAliveWindowMs` says so and it predates
// this module), so it is a fair floor rather than an idle one.


namespace coop::dev::death_write_diff {

// Capture the death-relevant object graph. GAME THREAD ONLY (reflection + raw property
// reads). Returns the number of (object, field) cells captured, or -1 if the scope could not
// be resolved. Replaces any previous snapshot.
int Snapshot();

// Diff the live graph against the last Snapshot() and LOG the raw delta -- one line per
// changed cell, plus died/appeared objects and per-class count deltas. `label` names the arm
// in the log. GAME THREAD ONLY. Returns the number of changed cells, or -1 if there is no
// snapshot to diff against.
//
// If `learnNoise` is true the changed (class, field) keys are RECORDED as the noise floor
// instead of being treated as findings; if false, any cell whose (class, field) key is in
// that floor is SUPPRESSED from the output and only counted. Call it once with true across
// the alive control window, then with false across the death.
int DiffAndLog(const char* label, bool learnNoise = false);

// Drop the learned noise floor (so a second scenario in one process starts clean).
void ResetNoiseFloor();

}  // namespace coop::dev::death_write_diff
