// coop/save/save_guard.h -- pre-session backup of the VOTV save directory.
//
// VOTV writes saves NON-ATOMICALLY: stock GameplayStatics::SaveGameToSlot truncates and
// overwrites the .sav in place -- four in-place writes per save through
// saveSlot_C::saveToSlot, with no temp-and-rename -- so a crash or a bad coop-era write
// leaves a corrupt save and the engine has no recovery of its own. A pre-session snapshot is
// therefore the ONLY recovery path.
//
// Persistence is host-only: the HOST's save is the canonical one being written during coop
// and clients are blocked from saving, their own save left untouched, so only the host needs
// the backup. The caller gates on role.
#pragma once

#include <filesystem>

namespace coop::save_guard {

// %LOCALAPPDATA%\VotV\Saved\SaveGames (empty path if LOCALAPPDATA is unset).
// Shared with coop/save/save_transfer -- the one save-dir resolver.
std::filesystem::path SaveGamesDir();

// Snapshot %LOCALAPPDATA%\VotV\Saved\SaveGames into a timestamped
// SaveGames\coop_backup\<YYYYMMDD_HHMMSS>\ directory, pruning to the newest few.
// Idempotent per process (first call wins). Synchronous filesystem work; call it
// off the game thread (the harness bringup thread) BEFORE the coop session starts
// injecting state, so the snapshot reflects the pre-coop save. No-op (logged) if
// the save dir is absent (e.g. a brand-new game).
void BackupSaveOnSessionStart();

}  // namespace coop::save_guard
