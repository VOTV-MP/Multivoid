// ue_wrap/core/paths.h -- the ONE owner of the install's directory anchors: data in ModDataDir(),
// diagnostics beside the exe.
//
// The mod's data (multivoid.ini + .example, multivoid_servers/, coop_players/, multivoid-players.txt,
// multivoid_thanks.txt, the identity key) lives in the mod's own data folder, ModDataDir(), so a
// copied game folder carries none of it and a manager's reinstall starts it clean. What stays
// beside the GAME EXE (ExeDir(), ...\VotV\Binaries\Win64) is a diagnostic or a file a player
// authors: multivoid.log, multivoid-loaded.txt, multivoid-compat-report.txt, coop-screenshots/,
// multivoid_reports/, multivoid_locale/. .github/ci/data_paths_gate.py holds the split.
// The module's location is loader-dependent (unreal_shimloader virtualizes Mods\ into the r2modman
// profile, where module-dir writes were measured landing): ModDataDir() resolves the real folder,
// and the exe dir is the one loader-independent home of the install for the rest.
//
// UE4SS's own log is the one artifact beside the loaded UE4SS module instead: Ue4ssModulePath and
// Ue4ssDir find that module.

#pragma once

#include <string>

namespace ue_wrap::paths {

// Directory containing the game executable (no trailing slash). Empty only on
// a GetModuleFileNameW failure (callers treat empty as "skip the write").
std::wstring ExeDir();

// Whether a file or folder named `name` (one path component) exists directly in ExeDir(): a pure
// test that logs nothing. False when the exe folder is unknown, and when it is the data folder (a
// file there is the mod's own, not a leftover). When it exists and `pathOut` is set, the full path
// is stored there. Only coop/mod_data/legacy_note asks it, for the files an older build kept beside
// the exe. Any thread.
bool ExistsBesideExe(const wchar_t* name, std::wstring* pathOut = nullptr);

// The mod's own data folder, the one home every file the mod keeps for itself is read from and written
// to: the PACKAGE folder that holds `dlls\main.dll` and `enabled.txt` (`Mods\Multivoid`), or, when a
// manager's shimloader launched the process (it exports SHIMLOADER_MOD_DIR, the profile's mod folder),
// `<SHIMLOADER_MOD_DIR>\<pkg>` -- the real profile path, since the module's own path is the logical one
// and a rename there is remapped. `<pkg>` is the folder above `dlls\` in the module's path. No trailing
// slash. Empty on any failure (no module path, the module not under a `dlls` folder, the profile's
// package folder missing), with one log line saying which; every caller treats empty as "no data
// folder". Safe on the boot thread from the first ini read: it uses the process environment and the
// module's path only, reads no config row, and resolves once (latched). Any thread.
std::wstring ModDataDir();

// The loaded UE4SS module's full path (the official "UE4SS.dll" or shimloader's lowercase
// "ue4ss.dll": the module lookup is case-insensitive) and its folder (no trailing slash). Both
// empty when no such module is loaded or its path cannot be read. Any thread.
std::wstring Ue4ssModulePath();
std::wstring Ue4ssDir();

}  // namespace ue_wrap::paths
