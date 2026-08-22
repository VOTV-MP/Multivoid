// ue_wrap/core/paths.h -- the ONE owner of the install-dir anchor.
//
// Every per-install runtime artifact (multivoid.log, multivoid.ini + .example,
// multivoid-loaded.txt, multivoid-players.txt, multivoid-banlist.txt,
// multivoid-compat-report.txt, coop-screenshots/, coop_players/) anchors on the
// GAME EXE's directory (...\VotV\Binaries\Win64), NOT on the mod DLL's own
// directory. The module's location is loader-dependent -- the retired xinput
// proxy mapped us beside the exe, UE4SS maps us at Mods\Multivoid\dlls\, and
// unreal_shimloader additionally virtualizes Mods\ into the r2modman profile
// (measured 2026-08-21: module-dir writes landed in the profile) -- while the
// exe dir is the one real, loader-independent home of the install. WP-2 of the
// D-3 UE4SS migration (votv-ue4ss-f2-migration-DESIGN-2026-08-21.md) dissolved
// the four per-file ModuleDir() copies into this single helper.

#pragma once

#include <string>

namespace ue_wrap::paths {

// Directory containing the game executable (no trailing slash). Empty only on
// a GetModuleFileNameW failure (callers treat empty as "skip the write").
std::wstring ExeDir();

}  // namespace ue_wrap::paths
