// coop/save/save_scratch.h -- where the mod keeps the save folders it makes for a session: the joiner's
// copy of the host's world (zcoop_<pid>) and the host's throwaway capture (zcoop_hostxfer).
//
// The game's slot menu lists every folder under <Saved>/SaveGames/ that holds a main.meta, so a copy kept
// there reads as one of the player's saves. These live beside SaveGames instead, in <Saved>/multivoid_session/.
// The game builds every save folder path as SaveGames/<name> (lib_C's "get save slot folder path"), so a
// name that steps out of SaveGames reaches them: FolderName gives that name, and it goes wherever the game
// takes a save folder's name. Game thread unless noted.

#pragma once

#include <filesystem>
#include <string>

namespace coop::save_scratch {

// <Saved>/multivoid_session; empty when the saved directory is unresolved. Any thread.
std::filesystem::path Root();

// The folder of scratch save `leaf` on disk. Any thread.
std::filesystem::path Dir(const std::wstring& leaf);

// The name the game and ue_wrap/engine/save_folder take for that folder: "../multivoid_session/<leaf>".
std::wstring FolderName(const std::wstring& leaf);

// Does a save folder path (as the save write hook hands it over) lie in the scratch root? True for the
// capture a join writes and a joiner's copy; false for every save of the player's.
bool IsScratch(const wchar_t* folderPath);

// Delete scratch save `leaf`, logged when something was there. Any thread.
void Remove(const std::wstring& leaf);

// Boot: delete the scratch saves a session that ended without cleaning up left behind. A joiner's
// zcoop_<pid> goes when that process is gone, a capture left by a host that died mid-join when it is a
// minute old; a live sibling's folder is never touched. Any thread.
void SweepStale();

}  // namespace coop::save_scratch
