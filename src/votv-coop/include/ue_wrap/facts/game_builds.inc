// ue_wrap/facts/game_builds.inc -- the game builds this Multivoid knows, oldest first.
//
// GAME_BUILD(id, version, display, exe link stamp, pak index hash) -- facts about the game only; the mod's own
// release history is coop's (coop/game_build/last_builds.inc).
//   id       names the row; sdk_profile.h's kTargetBuild names the target's, whose version is that same string
//   version  the game version line as a Multivoid target names it (CMake's VOTVCOOP_GAME_TARGET form)
//   display  the game's name for a person
//   stamp    IMAGE_FILE_HEADER::TimeDateStamp of the shipping exe, packed or not
//   hash     FPakInfo::IndexHash of Content/Paks/VotV-WindowsNoEditor.pak, 40 lowercase hex
//
// A new build is a row: boot the mod on it and copy the two values from its `game-build:` log line
// (docs/versioning.md, the retarget's identity step). Position orders the builds, so a row goes where its release
// falls. The Patreon early build is a row so the rig can prove the known-newer path on real bytes; its hashes
// disclose nothing beyond a build its developers announced. Included by ue_wrap/core/game_build.cpp only.

GAME_BUILD("0.9.0n", "0.9.0n", "0.9.0n", 0x6a08597du, "56065b874f3bfc685e67b17e96cfcbbad9c55248")
GAME_BUILD("0.10.0", "0.10.0", "0.10.0 alpha", 0x6ae37b05u, "e53f00da25b227693f0b0c8fdaed16b0eb72a164")
