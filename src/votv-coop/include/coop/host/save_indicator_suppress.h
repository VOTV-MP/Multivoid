// coop/save_indicator_suppress.h -- suppress the native "SAVED..." HUD indicator ONLY
// for the host's client-join scratch-save (save_transfer), never a manual F5/menu save.
//
// RE (research/findings + the 2026-06-24 agent RE): the join scratch-save is
// save_transfer.cpp -> save_capture::CaptureLiveWorldToScratchSlot -> mainGamemode.
// saveObjects, the ONLY direct saveObjects caller (a manual save uses the BP save()/
// autosave() path the mod never calls -> cleanly discriminable, synchronous game-thread,
// no interleave). The "SAVED..." text is painted by saveAnim / "Add Hint from Gamemode"
// (addHint) called BP-INTERNALLY inside saveObjects (EX_LocalVirtualFunction -> invisible
// to our ProcessEvent detour). We must NOT touch saveObjects/the disk write/the transfer
// -- only the cosmetic indicator, and only inside the join-save window.
//
// PHASE 1 (this build) = DETECT-LOG, READ-ONLY: a tight Begin/End flag brackets the
// join-save; post-hooks on saveAnim + addHint (via ue_wrap/ufunction_hook, which patches
// UFunction::Func below the BP VM) LOG which fires inside the window and FORWARD to the
// original (SAVED still shows). Confirms which one paints before the targeted no-op.
// PHASE 2 (next build, after the user reports which painted) = no-op that one in the
// window. Game thread only.
#pragma once

namespace coop::save_indicator_suppress {

// Raise the join-save flag immediately BEFORE the scratch-save capture (lazily installs
// the detect hooks on first call). Clear it immediately AFTER. Only saveAnim/addHint
// calls that land inside this synchronous window are detected (later: suppressed).
void Begin();
void End();

// Idempotent: resolve mainGamemode.saveAnim + "Add Hint from Gamemode" and install the
// detect post-hooks. Called lazily from Begin(); safe to call before the gamemode exists
// (no-op + retry next Begin).
void EnsureInstalled();

}  // namespace coop::save_indicator_suppress
