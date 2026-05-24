// harness/config.h -- env + ini configuration readers.
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

namespace harness::config {

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

// Build the net Config from env + ini. Sets `enabled` to true iff a
// host/client role is configured (otherwise hands-on play stays
// single-machine).
coop::net::Config ReadNetConfig(bool& enabled);

// The local player's display nickname. Env first, then ini, then "Player".
std::wstring ReadNickname();

}  // namespace harness::config
