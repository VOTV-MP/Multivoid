// coop/save_guard.h -- pre-session backup of the VOTV save directory.
//
// PR-FOUNDATION-2 (save-game safety) increment A. VOTV writes saves
// NON-ATOMICALLY: stock GameplayStatics::SaveGameToSlot truncates + overwrites
// the .sav in place (4 in-place writes per save via saveSlot_C:saveToSlot, no
// temp+rename), so a crash or a bad coop-era write leaves a corrupt save with
// NO engine-side recovery (RE: research/findings/votv-save-path-RE-2026-05-30.md).
// A pre-session snapshot is therefore the ONLY recovery path.
//
// Policy (host-only persistence, user decision 2026-05-30): the HOST's save is
// the canonical one being written during coop; clients are blocked from saving
// (their save is left untouched), so only the host needs the backup. The caller
// gates on role.
#pragma once

#include <filesystem>

namespace coop::save_guard {

// %LOCALAPPDATA%\VotV\Saved\SaveGames (empty path if LOCALAPPDATA is unset).
// Shared with coop/save_transfer (v56) -- the one save-dir resolver.
std::filesystem::path SaveGamesDir();

// Snapshot %LOCALAPPDATA%\VotV\Saved\SaveGames into a timestamped
// SaveGames\coop_backup\<YYYYMMDD_HHMMSS>\ directory, pruning to the newest few.
// Idempotent per process (first call wins). Synchronous filesystem work; call it
// off the game thread (the harness bringup thread) BEFORE the coop session starts
// injecting state, so the snapshot reflects the pre-coop save. No-op (logged) if
// the save dir is absent (e.g. a brand-new game).
void BackupSaveOnSessionStart();

}  // namespace coop::save_guard
