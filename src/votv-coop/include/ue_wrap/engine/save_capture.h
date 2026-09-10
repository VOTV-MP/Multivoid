// ue_wrap/engine/save_capture.h -- capture the host's LIVE world to a scratch save slot.
//
// Engine-wrapper layer (principle 7): pure engine-substrate access. It drives VOTV's own save
// populate (mainGamemode::saveObjects/saveTriggers) + the engine serializer
// (GameplayStatics::SaveGameToSlot). No network/gameplay logic here.
//
// WHY. save_transfer historically shipped the host's STALE on-disk .sav, so any entity the host
// changed AFTER its last autosave reached the joiner as it had been at that autosave -- a kerfur
// the host turned ON arrived as a turned-OFF prop, and the client's reconcile layer (divergence
// sweep + npc adoption) had to fight that staleness, duplicating or diverging the kerfur. The
// game's OWN save/load already round-trips a turned-on kerfur correctly, since it is an
// int_save_C actor and a single-player save/reload keeps it on. The only defect was snapshotting
// a stale file. This captures the host world LIVE at the instant a client joins, through the
// game's own save path, into a THROWAWAY slot -- so the joiner's native loadObjects() builds a
// world with nothing left to reconcile.

#pragma once

#include <string>

namespace ue_wrap::save_capture {

// Repopulate the host's in-memory world save (objects + triggers) from LIVE actors, then
// serialize it to `scratchSlotName` via GameplayStatics::SaveGameToSlot. The slot name is OURS (a
// transient zcoop_* file the caller deletes after reading), so the player's canonical slot is
// never named, opened, or written: this is not a real save and cannot clobber the host's
// progress. Player-state populates (playerTransform/inventory/heldObj) are deliberately skipped,
// because the joiner overrides those and each has its own live coop sync channel.
//
// Returns true iff the scratch .sav was written. GAME THREAD ONLY -- it calls mainGamemode
// UFunctions + the engine serializer, both ProcessEvent-dispatched.
bool CaptureLiveWorldToScratchSlot(const std::wstring& scratchSlotName);

}  // namespace ue_wrap::save_capture
