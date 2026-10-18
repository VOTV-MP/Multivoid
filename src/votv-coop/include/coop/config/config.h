// coop/config/config.h -- the env and ini configuration readers. multivoid.ini lives in the mod's
// data folder (ue_wrap::paths::ModDataDir); the LAN test framework overrides through environment
// variables, since one DLL location serves two instances and per-file configs would alias.
// Precedence: the session's value of a server-scope row while a session runs (the session layer),
// then a value set while the game runs (the runtime layer), then the environment variable (set by
// the test launcher), then the ini value, then the row default.

#pragma once

#include "coop/config/config_registry.h"
#include "coop/net/session.h"

#include <functional>
#include <string>
#include <vector>

namespace coop::config {

// An environment variable, whole and converted losslessly to UTF-8 (a control character kept, a
// lone surrogate as U+FFFD), so a caller that shows or trusts it sanitises it itself; empty if
// unset.
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

// The identity write door: one key=value line in multivoid.ini, by typed handle, with no reader
// and no layer. The authoritative line is the first case-insensitive occurrence of the key,
// edited in place with the canonical spelling; duplicates are left alone, other bytes stay as
// they are, and the rewritten line's inline comment is deleted. Best-effort: a read-only
// directory means the value is not remembered, logged and false; true means the atomic swap
// landed. Any value is accepted (ValueValidForKey is true for identity rows). The
// string-keyed machinery below (reformat, keep-line, skeleton) operates on keys
// discovered in the file.
bool WriteIniValue(const config_registry::IdentityRow& row, const char* value);

// What SetValue did. Refused: the value is one the reader would refuse, the call came from
// inside a subscriber, or a replicated row's value is longer than
// kServerSettingTextMax bytes; nothing changed. HeldNotSaved: held for this run, but the ini
// could not be written. Saved: held and written.
enum class SetResult : unsigned char { Refused, HeldNotSaved, Saved };

// Set a row's value while the game runs. The value is normalised as the ini writer normalises it
// (line breaks removed, edges trimmed unless the row is a String row, which keeps them), refused
// if the reader would refuse it, held in the runtime layer (which every Resolve reads above the
// environment), written to multivoid.ini, logged, and announced to the row's subscribers. Held
// means every later Resolve returns it (a server-scope row on a client in a session excepted: the
// session's value answers once the host's value has arrived); a reader that latched the row at
// its first use keeps what it latched until the next launch. Any thread; two sets are
// serialised.
// A server-scope row (config_registry::IsServerScope): on the host in a session the value also
// goes into the session layer, inside the setter and before its one notification, so the session
// sees it; on a client in a session the set changes the install's own hosting default and nothing
// the session sees. A replicated row's value longer than config_registry::kServerSettingTextMax
// is Refused.
SetResult SetValue(const config_registry::FlagRow& row, const char* value);
SetResult SetValue(const config_registry::IntRow& row, const char* value);
SetResult SetValue(const config_registry::FloatRow& row, const char* value);
SetResult SetValue(const config_registry::EnumRow& row, const char* value);
SetResult SetValue(const config_registry::StringRow& row, const char* value);

// Return a row to its default while the game runs: the runtime layer forgets it, its line leaves
// multivoid.ini (an environment twin, if set, answers again; on the host in a session, a
// server-scope row's session layer then holds what the layers below it answer), the reset is
// logged, and the row's subscribers are called -- what SetValue is to a chosen value, this is to
// the default. Refused from inside a subscriber. HeldNotSaved: the runtime layer forgot it, but
// the ini could not be rewritten, so the FILE's stored value answers again until it can (a reader
// sees the old value, not the default -- the drill's probe then prints `did not follow back`).
// Saved: forgotten and the line removed (or there was none). Any thread; serialised with SetValue.
SetResult ResetValue(const config_registry::FlagRow& row);
SetResult ResetValue(const config_registry::IntRow& row);
SetResult ResetValue(const config_registry::FloatRow& row);
SetResult ResetValue(const config_registry::EnumRow& row);
SetResult ResetValue(const config_registry::StringRow& row);

// A server setting named by a person (Source finds a cvar by its name): exactly SetValue /
// ResetValue for a row FindRow returned. Refused, with nothing changed, for a null row, a row not
// in the table, or a row that is not server-scope -- so a local row cannot be set by name.
SetResult SetServerRow(const config_registry::Row* row, const char* value);
SetResult ResetServerRow(const config_registry::Row* row);

// Call `onChange` after each SetValue or ResetValue of `row` that was not Refused; it re-resolves
// what it needs. It runs through the notifier (below): on the game thread in the game. Registering
// the same (row, function) pair again is a no-op. Any thread. Subscribe before the first read of
// the row; a set in between is then delivered. A subscriber never sets a row: a SetValue or
// ResetValue made synchronously on its own stack is refused, but a set it defers
// (game_thread::Post) or hands to another thread is not, and loops inside one drain, so a
// subscriber never posts or hands off a set either. On a client in a session, a set or reset of a
// server-scope row changes the install's own hosting default and nothing the session sees (its
// subscribers are still called and re-resolve the session's value). They are also called after
// each put into the session layer and when the layer is emptied.
void Subscribe(const config_registry::FlagRow& row, void (*onChange)());
void Subscribe(const config_registry::IntRow& row, void (*onChange)());
void Subscribe(const config_registry::FloatRow& row, void (*onChange)());
void Subscribe(const config_registry::EnumRow& row, void (*onChange)());
void Subscribe(const config_registry::StringRow& row, void (*onChange)());
// Follow every REPLICATED server-scope row with one function: the session module's sender. Any
// thread, once, at boot.
void SubscribeServerScope(void (*onChange)());

// How a change notification reaches its thread: `post` is handed a task that calls the row's
// subscribers. The game wires it to the game thread's queue at boot; a process that sets none
// has the subscribers called inside SetValue, on the setter's thread. Set once, before the first
// SetValue.
void SetNotifier(void (*post)(std::function<void()> task));

// The SESSION layer: the session's value of every server-scope row (config_registry.h), above the
// runtime layer while a session runs, on the host and on a client alike.
//
// A session opened. Any layer left by an earlier session is dropped first (a stale layer can only
// mean a stop seam that was missed, so it is logged as a warning). On the host, every server-scope
// row's value is read from the layers below the session (the runtime layer, the environment twin,
// the ini, the default), sanitised (a value the reader would refuse, or too long for a replicated
// row, becomes the row's default), held, logged `config: SESSION <key>=<v> (host start)` and
// announced; a stale row the new layer does not hold again is announced too, so its subscribers
// resolve the layer below. On a client the layer stays empty until the host's rows arrive. Any
// thread.
void SessionLayerBegin(bool host);
// A server-scope row's session value from the wire (a length-carried value: the wire's bytes are
// not NUL-terminated): validated as the reader validates, held, logged `(from the host)` and
// its subscribers told; a value the reader would refuse, or one that arrives with no client session
// running, is dropped with a warning. True when the value was held, false for either refusal. Game
// thread (the receiver's).
bool SessionLayerPut(const config_registry::Row* row, const std::string& text);
// The session ended: the layer empties and the role is none, logged `config: SESSION cleared`;
// every row it held tells its subscribers, so a consumer resolves the install's own value again.
// Any thread. Idempotent: with nothing held and no role it logs nothing.
void SessionLayerEnd();
// Game thread: the notify rows whose resolved value changed since the last take, in the order first
// marked; a session's begin and end clear them.
std::vector<const config_registry::Row*> TakePendingAnnouncements();

// The typed layered reads: Resolve(row) is the session's value of a server-scope row while a
// session runs (the session layer), then a value set while the game runs (the runtime layer),
// then the environment variable, then the ini value, then the row's default, validated
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
// Why a read of multivoid.ini failed. ReadFailed: the file exists but could not be opened or read
// whole (a lock, permissions, a mid-stream error). NotText: it holds a zero byte, or starts with a
// UTF-16 byte-order mark, as a file saved as UTF-16 does; no other encoding is detected. Either
// way the scan stops at the fault as Unreadable instead of reading past it: a default-taking row
// keeps a key read whole before the fault, as before a read error, and a fail-closed row refuses
// whatever was read (ResolveFailClosed).
enum class IniFault : unsigned char { None, ReadFailed, NotText };
// The words that follow "multivoid.ini" for a fault, for the texts that name it.
const char* IniFaultWords(IniFault fault);

// The fail-closed read (a CFG_ENUM_FAILCLOSED row; its handle type takes no other reader). Value:
// `out` holds the canonical token or the row default, exactly as ResolveEnum would give. Refused:
// a layer supplied a value the reader refuses; `refusedOut` holds it (possibly empty) and
// `originOut` the env var's name or multivoid.ini. Unreadable: no env value is set and
// multivoid.ini could not be read whole, so its answer is unknown even where a line was read before
// the fault; `originOut` is multivoid.ini and `faultOut` says why.
enum class FailClosedRead : unsigned char { Value, Refused, Unreadable };
FailClosedRead ResolveFailClosed(const config_registry::FailClosedEnumRow& row, std::string& out,
                                 std::string* refusedOut = nullptr,
                                 std::string* originOut = nullptr,
                                 IniFault* faultOut = nullptr);
// Free strings: the session's value of a server-scope row while a session runs (the session layer),
// then a value set while the game runs (the runtime layer), then the environment variable, then
// the ini value, then the row default; no validation.
std::string ResolveString(const config_registry::StringRow& row);
// The text every Resolve of this row starts from: the top layer that answers, or the default as
// text. Any thread.
std::string EffectiveText(const config_registry::Row& row);
// The census's rendering of `raw` as the row's resolved type -- a float `%.9g`, a flag `1`/`0` -- so
// two spellings of one value render as one text.
std::string ResolvedText(const config_registry::Row& row, const std::string& raw);
// True when `row` is notify and its resolved value differs between the two raw texts.
bool ShouldAnnounce(const config_registry::Row& row, const std::string& before,
                    const std::string& after);
// What a pane draws from EffectiveText, the readers' own steps exposed unchanged. FlagVerdict: 1 for
// 1/true/yes/on, -1 for 0/false/no/off, 0 for anything else (a reader answers the row's default);
// CookValue: the text cooked as the typed readers cook it, to compare with a row's tokens.
int FlagVerdict(const std::string& raw);
std::string CookValue(const std::string& raw);

// The net Config from the runtime layer, env and ini; `enabled` is true iff a host or client role
// is configured, otherwise hands-on play stays single-machine.
coop::net::Config ReadNetConfig(bool& enabled);

// The display nickname: a value set while the game runs (the runtime layer), then the environment
// variable, then the ini value, then the registry's my-name default.
std::wstring ReadNickname();

// The persisted body-skin choice (the ini's player_skin). Absent or invalid, the default is
// assigned and persisted.
std::string ReadPlayerSkin();

// The file operations behind the review panel and the boot sweep.

// All lines of the live ini, with trailing newlines kept. The scan code: 0 ok, 1 absent, 2
// unreadable, with `faultOut` saying why.
int ListLiveIniLines(std::vector<std::string>& out, IniFault* faultOut = nullptr);

// Reader-equivalent validation of a raw ini value for `key` against its registry row,
// comment-stripped exactly as the readers do. True for string and identity rows and for
// unregistered keys. On false the optional reason gets the panel-facing text. Shared by the
// writer and the boot sweep: one validation, never two.
bool ValueValidForKey(const char* key, const std::string& rawValue, std::string* reasonOut);

// The review panel's keep-line action: keep the first line of `key` whose comment-stripped
// value equals `keepValue`, drop every other occurrence. Correlated by value, never by line
// number, since the panel's snapshot ages and a stale index could delete the wrong copy of an
// identity key; refused when no current line carries the value. The automatic write path never
// deletes. An atomic swap. On success the key's runtime-layer value is dropped (the stored value
// answers again) and its subscribers are notified.
bool RemoveDuplicateKeyLines(const char* key, const char* keepValue);

// The review panel's opt-in reformat, never automatic. It collapses value-identical duplicate
// key lines (the first is kept); emits the registry sections in canonical order and places each
// single-occurrence known key under its section header, its attached comment block travelling
// with it; never repositions or adjudicates a key with differing duplicate values (it stays in
// the residue for the keep-line buttons), and unknown keys and loose comments keep their order
// in the residue tail; and retires an unknown key line or a known key whose value fails
// validation to a comment, so the panel's complaint resolves while the data stays readable in
// the file -- except a fail-closed row's (config_registry::Row::failClosed), which stays, since
// disabling it would hand the row the default its refusal withholds.
// `fault` says why, when an unreadable file refused the reformat.
struct ReformatStats {
    int collapsed = 0;
    int placed = 0;
    int frozen = 0;
    int retired = 0;
    IniFault fault = IniFault::None;
};
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

// Generate multivoid.ini.example in the mod's data folder: every registry row as wrapped description
// prose, the generator-emitted allowed tokens, range and env twin, and a copyable commented
// key=default line, under bare section headers. Deterministic bytes (no timestamp; numeric
// emission in the C locale); compare first; the one atomic-swap primitive; fail-soft, since the
// mod never reads this file back. Once at boot, after EnsureIniSkeleton.
void GenerateExampleCatalog();

// Say what this launch is actually configured with: one line per registry row a layer supplied,
// carrying the resolved value and the layer that won, then an end line with the count. Once at
// boot, after MigrateRetiredIniValues, so the census reports the state everything else will read.
// The line text is a contract with tools outside the tree -- a drill asserts its own independent
// variables against it, since a run whose setting silently did not take measures the wrong build.
void ReportEffectiveConfig();

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

// Identity and durability state, set during the boot mints: whether this launch's skin is
// session-only (the ini was unreadable at the mint, or the persist failed), and the fault of
// the last live-ini read that failed this launch (None while none did), in which case that read
// ran on env and defaults. Both feed the config review panel; the fault also the census.
bool IdentityNotDurable();
IniFault LastIniFault();

// Boolean flags.

// False only when the ini or its env twin holds an explicit falsy `enabled`, the dev master
// kill-switch; absent or garbage is true, and the granular switches decide.
bool MasterEnabled();

}  // namespace coop::config
