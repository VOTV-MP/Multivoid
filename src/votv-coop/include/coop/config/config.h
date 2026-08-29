// coop/config/config.h -- env + ini configuration readers.
//
// Both the scenario file (scenario.txt) and the user-facing ini
// (multivoid.ini) live next to the mod DLL. The LAN test framework
// overrides via environment variables (one DLL location serves two
// instances, so per-file configs would alias).
//
// Precedence (highest first):
//   1. Environment variable (lan-test framework)
//   2. multivoid.ini value
//   3. Hard-coded default

#pragma once

#include "coop/config/config_registry.h"
#include "coop/net/session.h"

#include <string>
#include <vector>

namespace coop::config {

// Read an environment variable (ASCII). Empty string if unset.
std::string ReadEnv(const char* name);

// Read the launch scenario: the VOTVCOOP_SCENARIO env var (per-launch signal),
// or "menu" on a native launch. (The on-disk scenario.txt fallback was RETIRED
// 2026-06-06 -- a leftover file aliased later native launches.)
std::string ReadScenario();

// (arc 3: the string-keyed ReadIniValue is INTERNAL now -- product reads go
// through the typed handle Resolve* APIs below; the schema's own machinery
// reads file-discovered keys via the config-internal twins.)

// Seed a fresh multivoid.ini SKELETON (ordered section headers from the
// registry, [net] first / [dev] last, plus the one user-ruled seeded-active
// line net.nick=<my-name default>). Runs ONLY when the ini is authoritatively
// ABSENT (ENOENT); an existing file -- readable or not -- is never touched.
// Atomic create; loses a concurrent-create race gracefully. Returns true if
// the skeleton was created. Call BEFORE the first ini write of a launch
// (harness boot, ahead of the guid/skin mints).
bool EnsureIniSkeleton();

// Create/update a single "key=value" line in multivoid.ini. TARGETING = the
// unified occurrence rule (ini rework T3): the authoritative line is the FIRST
// case-insensitive key occurrence, edited in place with canonical spelling; at
// N>1 duplicates only that line is edited; other bytes verbatim; the rewritten
// line's inline comment is deleted (it described the old value). Best-effort:
// a read-only dir just means the setting isn't remembered, never a crash (logs
// + returns false; true = the atomic swap landed). The ini is LOCAL-ONLY
// (gitignored). ASCII values.
//
// KEYED BY TYPED HANDLE (arc 3 C3b -- the write half of the ratchet): product
// code cannot persist an unregistered key. The VALUE stays a string: it is
// user-shaped UI output, refused by ValueValidForKey behind this door exactly
// as the reader would refuse it (T3b -- never persist garbage). The
// string-keyed machinery (reformat / keep-line / skeleton / selftests below)
// stays string-keyed by nature: it operates on keys discovered IN the file.
bool WriteIniValue(const config_registry::FlagRow& row, const char* value);
bool WriteIniValue(const config_registry::IntRow& row, const char* value);
bool WriteIniValue(const config_registry::FloatRow& row, const char* value);
bool WriteIniValue(const config_registry::EnumRow& row, const char* value);
bool WriteIniValue(const config_registry::StringRow& row, const char* value);
bool WriteIniValue(const config_registry::IdentityRow& row, const char* value);

// ---- typed layered reads (arc 2 T6; arc 3 = the const Row& ratchet) ---------
// Resolve(row) = env -> ini -> the ROW's default (T2-migrate: defaults live in
// the registry, sites pass no literals). Validation against the row's kind +
// range/tokens: one vocabulary for flags (1|true|yes|on / 0|false|no|off, ci);
// numbers must whole-parse AND land in [lo, hi]; enums must ci-match a token
// (the canonical token is returned). Anything else -- including
// present-but-empty (F33) -- is garbage: the row DEFAULT applies in memory
// (user ruling "дефолт ставить"), the T10 boot sweep reports it, nothing is
// written back. A SET env var that fails validation SHADOWS a valid ini value;
// an EMPTY env var is unset and falls through (F44). The env var name comes
// from the row. Handles are registry-minted only (config_registry.h): an
// unregistered key cannot be read -- the compiler is the gate.
bool        ResolveFlag(const config_registry::FlagRow& row);
long        ResolveInt(const config_registry::IntRow& row);
float       ResolveFloat(const config_registry::FloatRow& row);
std::string ResolveEnum(const config_registry::EnumRow& row);
// Free strings: env -> ini -> row default, no value validation (arc 3).
std::string ResolveString(const config_registry::StringRow& row);

// Build the net Config from env + ini. Sets `enabled` to true iff a
// host/client role is configured (otherwise hands-on play stays
// single-machine).
coop::net::Config ReadNetConfig(bool& enabled);

// The master/lobby server URL ("host:port"). Precedence: env (the net.master row's
// env twin) -> the custom-master gate (net.master.custom=1 -> ini net.master) -> the
// official endpoint (coop::net::kOfficialMasterUrl, which is also the row default).
// Pushed into session_manager at boot so a native (no-env, no-ini) launch points the
// browser/host flow at the VPS out of the box; set net.master.custom=1 +
// net.master=... to run your own master.
std::string ReadMasterUrl();

// A forced P2P-host transport Config (signaling/identity/stun from the same
// env/ini keys as ReadNetConfig's P2P path). The menu Host-Game flow falls back
// to this when the master announce fails, so hosting never silently dies on an
// unreachable master (RULE 1). Distinct from ReadNetConfig: does NOT read
// net.role (it must never trigger the play-path auto-start).
coop::net::Config ReadP2PHostFallback();

// The local player's display nickname. Env first, then ini, then the registry
// my-name default (config_registry::kMyNameDefault).
std::wstring ReadNickname();

// v144: ReadPlayerGuid is RETIRED (RULE 2), and with it the `player_guid=` ini
// line. The durable identity is now an Ed25519 keypair in multivoid_identity.key
// beside this file (coop/net/peer_identity.h), and the 32-hex guid every store is
// keyed by is DERIVED from its public key rather than minted here. The two could
// not coexist: a random guid in a plaintext ini is a bearer token -- copy the line
// and you ARE that player -- which is exactly the finding (security A15) the key
// replaces. The migration cost is stated in PLAN_01 s5: a host's stored inventory
// rows for VISITING players are orphaned once, at this bump.

// v93 skins: the persisted body-skin choice, stored beside the identity
// (multivoid.ini "player_skin="). Absent/invalid -> the default (the current
// scientist, skin_registry::kDefaultSkinName) is assigned + persisted.
std::string ReadPlayerSkin();

// ---- T10 sweep / T1b owner-reformat file operations (arc 2) -----------------

// All lines of the live multivoid.ini, verbatim (trailing newlines kept).
// Returns the scan code: 0 = Ok, 1 = Absent (ENOENT), 2 = Unreadable.
int ListLiveIniLines(std::vector<std::string>& out);

// Reader-equivalent validation of a raw ini value for `key` against its
// registry row (kind + range/tokens, comment-stripped exactly like the
// readers). True for String/Identity rows and unregistered keys. On false,
// `reasonOut` (optional) gets the panel-facing reason ("not a whole number in
// [1, 65535]", ...). Shared by the T3b writer and the T10 sweep -- ONE
// validation, never two.
bool ValueValidForKey(const char* key, const std::string& rawValue, std::string* reasonOut);

// T1b owner action (review panel "keep line" button): keep the FIRST line of
// `key` whose comment-stripped value equals `keepValue`; drop every other
// ci-occurrence. Correlated by VALUE, never by line number -- the panel's
// snapshot ages, and a stale index could delete the wrong (or every) copy of
// an identity key; refuses when no current line carries the chosen value.
// The automatic write path never deletes; this is the owner-triggered
// resolution of a differing-duplicate report. Atomic swap, same guards.
bool RemoveDuplicateKeyLines(const char* key, const char* keepValue);

// T1b owner opt-in reformat (review panel button; NEVER automatic):
//   - collapses value-identical duplicate key lines (keep the first);
//   - emits the registry sections in canonical order ([net] first, [dev]
//     last) and places each N==1 known key under its section header, its
//     immediately-attached comment block traveling with it;
//   - a key with N>1 DIFFERING values is never repositioned and never
//     adjudicated (stays in the residue, relative order kept -- resolve via
//     the keep-line buttons); unknown keys and loose comments keep original
//     order in the residue tail.
//   - an UNKNOWN key line (nothing in the registry reads it) and a single-
//     occurrence known key whose value FAILS typed validation are RETIRED to
//     comments ("; unknown key (tidy): ..." / "; invalid value (tidy): ...")
//     -- the review panel's complaint is resolved while the user's data stays
//     readable in the file (fix 2026-07-26: Tidy used to move layout only, so
//     the panel's rows survived every press and it looked dead).
struct ReformatStats { int collapsed = 0; int placed = 0; int frozen = 0; int retired = 0; };
bool ReformatLiveIni(ReformatStats& out);

// ---- the T8 catalog: multivoid.ini.example (arc 4) --------------------------

// Per-boot outcome of the catalog generation (the drill's FIRST assert -- a
// failed boot write must fail the drill regardless of surviving old bytes).
enum class ExampleGen : unsigned char {
    NotRun = 0,         // GenerateExampleCatalog never ran this boot
    Regenerated,        // bytes differed (or file absent) -> atomic swap landed
    UpToDate,           // existing bytes identical -> no write
    FailedWrite,        // the swap failed (disk/perms) -- WARN logged, non-fatal
    SkippedUnreadable,  // existing file present but unreadable -- no doomed swap
};

// Generate multivoid.ini.example beside the DLL: every registry row as wrapped
// `;; ` description prose (+ generator-emitted allowed-tokens/range/env-twin
// lines from the row columns) and a copyable `; key=default` line, under bare
// [section] headers. Deterministic bytes (no timestamp; numeric emission
// pinned to the C locale); tri-state compare-first; the ONE atomic-swap
// primitive; fail-soft (the mod NEVER reads this file back). Call once at
// harness boot after EnsureIniSkeleton.
void GenerateExampleCatalog();

// This boot's generation outcome (+ the emitted key count when green).
ExampleGen ExampleGenStatus(int* keyCountOut);

// Selftest verify of a generated catalog file (the arc-4 drill; probes are
// RULE-2-exempt): runs the six detectors (grammar/wrap/tri-directional
// exactly-once/env-only/orphan/section-placement) and the round-trip
// (strip the leading "; " from every copyable line -> `scratchPath` -> the
// ONE lexer -> FOUND + typed-equal vs the row defaults via the product
// cores). Returns the failure count (0 = green); each failure logs one
// "config-selftest: catalog FAIL ..." line.
int SelftestExampleVerify(const std::wstring& examplePath, const std::wstring& scratchPath);

// ---- T10 identity/durability state (set during the boot mints) --------------
// True when this launch's guid/skin is SESSION-ONLY: the ini was UNREADABLE at
// the mint (the gate refused to write over it -- the lock-release overwrite
// race) or the mint's persist failed. / True when ANY live-ini access hit
// UNREADABLE this launch (the ini layer dropped out for that read; the launch
// runs on env+defaults there). Both feed the config review panel's rows.
bool IdentityNotDurable();
bool IniUnreadableSeen();

// ---- boolean ini flags ------------------------------------------------------

// Returns false ONLY if multivoid.ini (or the env twin) holds an explicit
// falsy `enabled` -- the [dev] master kill-switch. Absent/garbage = true
// (granular switches decide). Arc 3: rides rows::enabled (def=true); the
// string-keyed IsIniKeyTrue is GONE -- flag reads go through
// ResolveFlag(rows::<flag>).
bool MasterEnabled();

// ---- dev selftest seams (config corpus instrument; probes are RULE-2-exempt) ----
// Path-parameterized twins of the two readers + the raw line list + a failing-
// source scan: the env-gated autotest (VOTVCOOP_RUN_CONFIG_SELFTEST) runs the
// REAL lexer over corpus ini files and proves the tri-state branches. Not for
// product use -- product code reads only the module-dir ini via the API above.
// `scan` codes: 0 = Ok (clean end of stream), 1 = Absent (ENOENT),
// 2 = Unreadable (open failure other than ENOENT, or mid-stream read error).
struct IniSelftestRead {
    int scan = 0;
    bool found = false;
    std::string value;
};
IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key);
int SelftestFlagTriState(const std::wstring& path, const char* key);
// Typed-resolver twins (arc 3 C5): the INI + DEFAULT halves of the layered
// resolve over `path` -- same per-kind validate/default cores as the live
// Resolve* by construction (shared *FromRaw functions), NO env layer. The env
// layer is drilled by its own control on the LIVE resolver (a dedicated
// env-twinned row via SetEnvironmentVariableA); the live resolve's absent
// path is untestable directly (the module ini exists on a rig).
bool        SelftestResolveFlagAt(const std::wstring& path, const config_registry::FlagRow& row);
long        SelftestResolveIntAt(const std::wstring& path, const config_registry::IntRow& row);
float       SelftestResolveFloatAt(const std::wstring& path, const config_registry::FloatRow& row);
std::string SelftestResolveEnumAt(const std::wstring& path, const config_registry::EnumRow& row);
std::string SelftestResolveStringAt(const std::wstring& path, const config_registry::StringRow& row);
int SelftestListLines(const std::wstring& path, std::vector<std::string>& out);
int SelftestScanWithFailure(int failAfterLines);
bool SelftestWriteValue(const std::wstring& path, const char* key, const char* value);
bool SelftestRemoveDuplicates(const std::wstring& path, const char* key, const char* keepValue);
bool SelftestReformat(const std::wstring& path, ReformatStats& stats);

}  // namespace coop::config
