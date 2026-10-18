// coop/mod_data/legacy_note.h -- the mod's own data folder, as seen from the exe folder it left:
// the one notice for a file an older build kept beside the executable, and the one warning for a
// session with no data folder.
//
// The mod's data (the ini, the server profile, the player profiles, the seen list, the thanks
// cache, the identity key) lives in ue_wrap::paths::ModDataDir(). A file of the same name beside
// the exe is read by nothing now and is the player's: this module reads, moves and deletes none of
// it, and says once per name that it is being ignored and where the data lives instead. It goes,
// with its callers and ue_wrap::paths::ExistsBesideExe, two releases after the move ships.

#pragma once

#include <cstdint>

namespace coop::legacy_note {

// The files an older build kept beside the exe. One owner calls Once for each, at the one site that
// reads its data.
enum class LegacyFile : uint8_t {
    Ini,
    IniExample,
    Servers,
    CoopPlayers,
    SeenPlayers,
    Thanks,
    BanList,
    IdentityKey,
};

// When `name` exists beside the exe (and the exe folder is not itself the data folder), logs once
// per (file, name): INFO "legacy <name> beside the exe -- ignoring; this data now lives in
// <the data folder>" (with no data folder: "... ignoring; the mod has no data folder to hold
// it"), and for IdentityKey, whose loss is a reset identity, a WARN naming the file's full path.
// A name already noted costs no stat; one that is absent is asked again. An entry holds at most
// two names (the identity key and the per-account key). Any thread.
void Once(LegacyFile file, const wchar_t* name);

// Logs once, WARN, when ue_wrap::paths::ModDataDir() is empty: the session runs on defaults and
// saves nothing of the mod's. Silent otherwise. Any thread.
void WarnIfNoDataFolder();

}  // namespace coop::legacy_note
