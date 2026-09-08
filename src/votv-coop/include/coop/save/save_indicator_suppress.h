// coop/save/save_indicator_suppress.h -- watch for the native "SAVED..." HUD indicator during
// the host's client-join scratch-save (save_transfer), and never a manual F5 or menu save.
//
// The join scratch-save is save_transfer.cpp -> save_capture::CaptureLiveWorldToScratchSlot ->
// mainGamemode.saveObjects, the ONLY direct saveObjects caller: a manual save takes the BP
// save()/autosave() path the mod never calls, so the two are cleanly discriminable.
//
// It DETECTS ONLY -- nothing here suppresses the indicator. The candidate painters, saveAnim
// and "Add Hint from Gamemode" (addHint), are called BP-INTERNALLY inside saveObjects
// (EX_LocalVirtualFunction, invisible to the ProcessEvent detour), so they are reached through
// ue_wrap/ufunction_hook, which patches UFunction::Func below the BP VM; both post-hooks
// forward to the original. A live run caught NEITHER firing while the indicator showed, so
// either the painter is a third path or the Func patch does not reach these two, and until
// that is settled there is nothing to no-op. saveObjects, the disk write and the transfer are
// never touched. Game thread only.
#pragma once

namespace coop::save_indicator_suppress {

// Raise the join-save flag immediately BEFORE the scratch-save capture (lazily installs the
// detect hooks on first call). Clear it immediately AFTER. Only saveAnim/addHint calls that
// land inside this synchronous window, or within ~3 s of its close, are reported.
void Begin();
void End();

// Idempotent: resolve mainGamemode.saveAnim + "Add Hint from Gamemode" and install the
// detect post-hooks. Called lazily from Begin(); safe to call before the gamemode exists
// (no-op + retry next Begin).
void EnsureInstalled();

}  // namespace coop::save_indicator_suppress
