// coop/config/config.h -- env + ini configuration readers.
//
// Both the scenario file (scenario.txt) and the user-facing ini
// (votv-coop.ini) live next to the mod DLL. The LAN test framework
// overrides via environment variables (one DLL location serves two
// instances, so per-file configs would alias).
//
// Precedence (highest first):
//   1. Environment variable (lan-test framework)
//   2. votv-coop.ini value
//   3. Hard-coded default

#pragma once

#include "coop/net/session.h"

#include <string>

namespace coop::config {

// Directory of this mod DLL (its containing folder; no trailing slash).
std::wstring ModuleDir();

// Read an environment variable (ASCII). Empty string if unset.
std::string ReadEnv(const char* name);

// Read scenario string. Env override (VOTVCOOP_SCENARIO) -> scenario.txt
// -> "newgame" default.
std::string ReadScenario();

// Read a single "key=value" line from votv-coop.ini. Section-agnostic.
// Returns `def` if the key is absent.
std::string ReadIniValue(const char* key, const char* def);

// Create/update a single "key=value" line in votv-coop.ini (matches ReadIniValue's
// section-agnostic lookup). Persists local server-browser settings -- the player
// name + the last direct-connect address -- so they survive a relaunch. Best-effort:
// a read-only dir just means the setting isn't remembered, never a crash (logs +
// returns). The ini is LOCAL-ONLY (gitignored). ASCII values.
void WriteIniValue(const char* key, const char* value);

// Build the net Config from env + ini. Sets `enabled` to true iff a
// host/client role is configured (otherwise hands-on play stays
// single-machine).
coop::net::Config ReadNetConfig(bool& enabled);

// The master/lobby server URL ("host:port"). Precedence: env VOTVCOOP_MASTER_URL ->
// the custom-master gate (net.master.custom=1 -> ini net.master) -> the BUILT-IN VPS
// endpoint hardcoded in config.cpp (kBuiltinMasterUrl). Pushed into session_manager at
// boot so a native (no-env, no-ini) launch points the browser/host flow at the VPS out
// of the box; set net.master.custom=1 + net.master=... to run your own master.
std::string ReadMasterUrl();

// A forced P2P-host transport Config (signaling/identity/stun from the same
// env/ini keys as ReadNetConfig's P2P path). The menu Host-Game flow falls back
// to this when the master announce fails, so hosting never silently dies on an
// unreachable master (RULE 1). Distinct from ReadNetConfig: does NOT read
// net.role (it must never trigger the play-path auto-start).
coop::net::Config ReadP2PHostFallback();

// The local player's display nickname. Env first, then ini, then "Player".
std::wstring ReadNickname();

// The local player's durable identity GUID (32 lowercase hex chars) for the host-side
// per-player inventory file. Read from votv-coop.ini "player_guid="; generated + persisted
// on first launch / if absent/malformed. Per-install identity (design 2.3).
std::string ReadPlayerGuid();

// v93 skins: the persisted body-skin choice, stored next to the guid
// (votv-coop.ini "player_skin="). Absent/invalid -> the default (the current
// scientist, skin_registry::kDefaultSkinName) is assigned + persisted.
std::string ReadPlayerSkin();

// ---- boolean ini flags (merged from coop/session/ini_config, 2026-07-10) ----

// Returns false ONLY if votv-coop.ini contains `enabled=0` (or `enabled=false`)
// -- the [dev] master kill-switch. Missing key or =1 returns true.
bool MasterEnabled();

// Read a `key=1` / `key=true` style flag line. Case/space/inline-comment
// tolerant. Returns false if the file is missing, the key is absent, or the
// key is set to 0/false.
bool IsIniKeyTrue(const char* key);

}  // namespace coop::config
