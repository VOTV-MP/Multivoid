// ue_wrap/engine/save_capture.h -- capture the host's LIVE world to a scratch save folder.
//
// Engine-wrapper layer (principle 7): pure engine-substrate access. It drives VOTV's own world
// gather (mainGamemode::saveObjects) and the game's compressed writer (CompressedSaveLoadLib, through
// ue_wrap/engine/save_folder). No network/gameplay logic here.
//
// WHY. Shipping the host's on-disk save hands a joiner the world as it was at the host's last
// save: a kerfur the host turned on since arrives as a turned-off prop, and the client's reconcile
// layer has to fight that staleness. The game's own save/load round-trips such a world correctly,
// so this captures the host world LIVE at the instant a client joins, through the game's own gather,
// into a THROWAWAY folder -- and the joiner's native loadObjects builds a world with nothing left to
// reconcile.
#pragma once

#include <string>

namespace ue_wrap::save_capture {

// Repopulate the host's in-memory world save from LIVE actors (saveObjects with its event test
// skipped), then write the files a load needs into SaveGames/<folderName>/: the world save, persistent.sav
// and main.meta (the mode, rules and starter map). The world save is main.sav, except for a host standing
// in an area, whose live save is the area's: that goes to areas/<area>.sav, beside a copy of the host's
// own main.sav, and `area` names it (empty outside an area). The folder is OURS (a transient scratch folder
// the caller deletes after reading), so the player's own save is never written. The capture carries the
// host's own player state (the save object is its live store); the joiner replaces the per-player part on
// its side, before its world is built. False when a save object is missing, the gather produced no object
// records, or a write failed.
// GAME THREAD ONLY.
bool CaptureLiveWorldToFolder(const std::wstring& folderName, std::wstring& area);

// ---- the moment the world is gathered ----------------------------------------------------------
//
// mainGamemode::saveObjects(quicksave, skipEventBlock) does not always gather: without
// skipEventBlock, while lib_C::getEvent says an event is on, it returns without touching the object,
// primitive and trigger records, and whatever is serialized next -- the game's own save included --
// carries the world of the LAST gather. So "a save was written" and "the world in it is current"
// are two different moments, and state that has to stay consistent with the saved world
// (coop/player/player_profile_store) is tied to this one.
//
// The hook is raised when saveObjects is about to gather -- told to skip its event test (the
// capture above always is), or answered "no event" -- and not when the test sends it past the
// gather. Game thread, inside the saveObjects call.
using WorldGatherFn = void (*)();
void SetWorldGatherHook(WorldGatherFn fn);

// Register the watches the hook stands on (saveObjects' entry, and lib_C::getEvent as it calls it). Idempotent;
// false while lib_C or mainGamemode_C is not loaded, or the script gate did not install. The
// capture calls it itself; a consumer of the hook calls it at world-up so the game's own saves are
// seen from the first one. Game thread.
bool InstallGatherWatch();

// Does the game count an event as running right now (lib_C::getEvent)? Three answers, because
// "cannot be asked" is not "no": a caller that would act on "no event" must not act on Unknown.
// Independent of the watch above. Game thread.
enum class EventState { Unknown, Off, On };
EventState GameEventState();

}  // namespace ue_wrap::save_capture
