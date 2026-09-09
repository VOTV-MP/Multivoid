// coop/config/config.h -- the env and ini configuration readers. multivoid.ini lives next to the
// mod DLL; the LAN test framework overrides through environment variables, since one DLL location
// serves two instances and per-file configs would alias. Precedence: the environment variable (set
// by the test launcher), then the ini value, then the row default.

#pragma once

#include "coop/config/config_registry.h"
#include "coop/net/session.h"

#include <string>
#include <vector>

namespace coop::config {

// An environment variable (ASCII); empty if unset.
std::string ReadEnv(const char* name);

// The launch scenario: the VOTVCOOP_SCENARIO env var, or menu on a native launch.
std::string ReadScenario();

// The string-keyed ini read is internal; product reads go through the typed Resolve functions
// below.

// Seed a fresh multivoid.ini skeleton: the ordered section headers from the registry and the
// one seeded-active line, net.nick. Runs only when the ini is authoritatively absent; an
// existing file, readable or not, is never touched. An atomic create that loses a
// concurrent-create race gracefully. True if created. Call before the first ini write of a
// launch.
bool EnsureIniSkeleton();

// Create or update one key=value line in multivoid.ini. The authoritative line is the first
// case-insensitive occurrence of the key, edited in place with the canonical spelling;
// duplicates are left alone, other bytes stay as they are, and the rewritten line's inline
// comment is deleted, since it described the old value. Best-effort: a read-only directory
// means the setting is not remembered, logged and false; true means the atomic swap landed.
// ASCII values. Keyed by typed handle, so product code cannot persist an unregistered key; the
// value stays a string, refused by ValueValidForKey exactly as the reader would refuse it. The
// string-keyed machinery below (reformat, keep-line, skeleton, selftests) operates on keys
// discovered in the file.
bool WriteIniValue(const config_registry::FlagRow& row, const char* value);
bool WriteIniValue(const config_registry::IntRow& row, const char* value);
bool WriteIniValue(const config_registry::FloatRow& row, const char* value);
bool WriteIniValue(const config_registry::EnumRow& row, const char* value);
bool WriteIniValue(const config_registry::StringRow& row, const char* value);
bool WriteIniValue(const config_registry::IdentityRow& row, const char* value);

// The typed layered reads: Resolve(row) is env, then ini, then the row's default, validated
// against the row's kind and range or tokens. One vocabulary for flags (1, true, yes, on and
// their negations, case-insensitive); a number must parse whole and land in range; an enum must
// match a token case-insensitively, and the canonical token is returned. Anything else, an
// empty value included, is garbage: the row default applies in memory, the boot sweep reports
// it, nothing is written back. A set env var that fails validation shadows a valid ini value;
// an empty env var is unset and falls through. Handles are registry-minted, so an unregistered
// key cannot be read.
bool        ResolveFlag(const config_registry::FlagRow& row);
long        ResolveInt(const config_registry::IntRow& row);
float       ResolveFloat(const config_registry::FloatRow& row);
std::string ResolveEnum(const config_registry::EnumRow& row);
// Free strings: env, ini, row default, no validation.
std::string ResolveString(const config_registry::StringRow& row);

// The net Config from env and ini; `enabled` is true iff a host or client role is configured,
// otherwise hands-on play stays single-machine.
coop::net::Config ReadNetConfig(bool& enabled);

// The master server URL. Precedence: the env twin of the net.master row, then the
// custom-master gate (net.master.custom=1 selects the ini's net.master), then the official
// endpoint, coop::net::kOfficialMasterUrl, which is also the row default. Pushed into
// session_manager at boot, so a native launch points the browser at the official master out of
// the box.
std::string ReadMasterUrl();

// A forced P2P-host transport Config from the same keys as ReadNetConfig's P2P path. The
// Host-Game flow falls back to it when the master announce fails, so hosting never dies on an
// unreachable master. It does not read net.role, so it never triggers the play-path
// auto-start.
coop::net::Config ReadP2PHostFallback();

// The display nickname: env, then ini, then the registry's my-name default.
std::wstring ReadNickname();

// The persisted body-skin choice (the ini's player_skin). Absent or invalid, the default is
// assigned and persisted.
std::string ReadPlayerSkin();

// The file operations behind the review panel and the boot sweep.

// All lines of the live ini, with trailing newlines kept. The scan code: 0 ok, 1 absent, 2
// unreadable.
int ListLiveIniLines(std::vector<std::string>& out);

// Reader-equivalent validation of a raw ini value for `key` against its registry row,
// comment-stripped exactly as the readers do. True for string and identity rows and for
// unregistered keys. On false the optional reason gets the panel-facing text. Shared by the
// writer and the boot sweep: one validation, never two.
bool ValueValidForKey(const char* key, const std::string& rawValue, std::string* reasonOut);

// The review panel's keep-line action: keep the first line of `key` whose comment-stripped
// value equals `keepValue`, drop every other occurrence. Correlated by value, never by line
// number, since the panel's snapshot ages and a stale index could delete the wrong copy of an
// identity key; refused when no current line carries the value. The automatic write path never
// deletes. An atomic swap.
bool RemoveDuplicateKeyLines(const char* key, const char* keepValue);

// The review panel's opt-in reformat, never automatic. It collapses value-identical duplicate
// key lines (the first is kept); emits the registry sections in canonical order and places each
// single-occurrence known key under its section header, its attached comment block travelling
// with it; never repositions or adjudicates a key with differing duplicate values (it stays in
// the residue for the keep-line buttons), and unknown keys and loose comments keep their order
// in the residue tail; and retires an unknown key line or a known key whose value fails
// validation to a comment, so the panel's complaint resolves while the data stays readable in
// the file.
struct ReformatStats { int collapsed = 0; int placed = 0; int frozen = 0; int retired = 0; };
bool ReformatLiveIni(ReformatStats& out);

// The catalog: multivoid.ini.example.

// The per-boot outcome of the catalog generation; the drill's first assert, since a failed boot
// write must fail it regardless of surviving old bytes.
enum class ExampleGen : unsigned char {
    NotRun = 0,         // GenerateExampleCatalog never ran this boot
    Regenerated,        // bytes differed (or file absent) -> atomic swap landed
    UpToDate,           // existing bytes identical -> no write
    FailedWrite,        // the swap failed (disk/perms) -- WARN logged, non-fatal
    SkippedUnreadable,  // existing file present but unreadable -- no doomed swap
};

// Generate multivoid.ini.example beside the DLL: every registry row as wrapped description
// prose, the generator-emitted allowed tokens, range and env twin, and a copyable commented
// key=default line, under bare section headers. Deterministic bytes (no timestamp; numeric
// emission in the C locale); compare first; the one atomic-swap primitive; fail-soft, since the
// mod never reads this file back. Once at boot, after EnsureIniSkeleton.
void GenerateExampleCatalog();

// Retire a stored value a shipped bug wrote; once at boot, after EnsureIniSkeleton. A migration
// rather than a new default: browser.lastdirect once prefilled the direct-connect box with
// 127.0.0.1:7777, Unreal's default port and never ours (a host listens on
// coop::net::kDefaultPort), and the browser writes the row on focus loss with nothing typed, so
// a player who merely clicked the box has the dead port burned into the ini, where a default
// change is invisible. Exact match only, and once: the row is rewritten iff it still equals
// the retired literal, so a value the player chose, a deliberate :7777 included, is never
// touched.
void MigrateRetiredIniValues();

// This boot's generation outcome, plus the emitted key count when green.
ExampleGen ExampleGenStatus(int* keyCountOut);

// The selftest verification of a generated catalog: the detectors (grammar, wrap, exactly-once
// in each direction, env-only, orphan, section placement) and the round trip (every copyable
// line uncommented into `scratchPath`, through the one lexer, found and typed-equal to the row
// defaults). Returns the failure count; each failure logs one catalog FAIL line.
int SelftestExampleVerify(const std::wstring& examplePath, const std::wstring& scratchPath);

// Identity and durability state, set during the boot mints: whether this launch's guid or skin
// is session-only (the ini was unreadable at the mint, or the persist failed), and whether any
// live-ini access hit an unreadable file this launch, in which case that read ran on env and
// defaults. Both feed the config review panel.
bool IdentityNotDurable();
bool IniUnreadableSeen();

// Boolean flags.

// False only when the ini or its env twin holds an explicit falsy `enabled`, the dev master
// kill-switch; absent or garbage is true, and the granular switches decide.
bool MasterEnabled();

// The dev selftest seams: path-parameterised twins of the readers, the raw line list and a
// failing-source scan, so the env-gated autotest runs the real lexer over corpus ini files and
// proves the tri-state branches. Not for product use. `scan` codes: 0 ok, 1 absent, 2
// unreadable (an open failure other than absence, or a mid-stream read error).
struct IniSelftestRead {
    int scan = 0;
    bool found = false;
    std::string value;
};
IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key);
int SelftestFlagTriState(const std::wstring& path, const char* key);
// The typed-resolver twins: the ini and default halves of the layered resolve over `path`, the
// same per-kind validate and default cores as the live Resolve functions, with no env layer.
// The env layer is drilled by its own control on the live resolver through a dedicated
// env-twinned row.
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
