// ue_wrap/core/paths.h -- the ONE owner of the two directory anchors: the install, and the
// player's profile.
//
// Every per-install runtime artifact (multivoid.log, multivoid.ini + .example,
// multivoid-loaded.txt, multivoid-players.txt, multivoid-banlist.txt,
// multivoid-compat-report.txt, coop-screenshots/, coop_players/) anchors on the
// GAME EXE's directory (...\VotV\Binaries\Win64), NOT on the mod DLL's own directory.
// The module's location is loader-dependent: UE4SS maps us at Mods\Multivoid\dlls\, and
// unreal_shimloader additionally virtualizes Mods\ into the r2modman profile, where
// module-dir writes were measured landing. The exe dir is the one real, loader-independent
// home of the install, so every artifact above resolves its directory through this one
// helper rather than computing it per file. The profile anchor below is the fallback for an
// account that cannot own an install's own key file.

#pragma once

#include <string>

namespace ue_wrap::paths {

// Directory containing the game executable (no trailing slash). Empty only on
// a GetModuleFileNameW failure (callers treat empty as "skip the write").
std::wstring ExeDir();

// The Multivoid folder under this Windows account's local application data, created on
// first use (no trailing slash): the home of an account's own key for an install whose file
// it cannot own. Empty when the folder can be neither found nor created (callers treat empty
// as "skip the write").
std::wstring ProfileDir();

}  // namespace ue_wrap::paths
