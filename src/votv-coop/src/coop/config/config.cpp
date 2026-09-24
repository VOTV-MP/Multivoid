// coop/config/config.cpp -- the env and ini configuration readers: the one unbounded ini line
// primitive with its tri-state scan verdict, the occurrence rule and the two value layers, the
// typed layered reads off the registry rows, and the net, nickname and skin readers.

#include "coop/config/config.h"

#include "config_internal.h"
#include "ue_wrap/core/paths.h"
#include "coop/session/player_handshake.h"  // kNickMaxChars
#include "coop/text/utf8_codec.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/skin_registry.h"  // IsValidSkinName and PickRandomStarterSkin
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace coop::config {

std::string ReadEnv(const char* name) {
    // The environment is read wide and re-encoded to UTF-8: Windows keeps the block as UTF-16, and
    // the narrow API converts to the process ANSI codepage, so a Cyrillic nickname arrived as
    // cp1251 bytes and every UTF-8 layer above rendered it as a row of U+FFFD. The rest of the
    // config stack speaks UTF-8, so this is the one boundary.
    // The value is read whole, whatever its length, and converted losslessly: a validated row must
    // see exactly what was set, so an over-long value is not an unset one and a control character
    // is not quietly dropped into a valid token. A consumer that needs text sanitised (the
    // nickname) does it itself. A lone surrogate becomes U+FFFD.
    const std::wstring wname(name, name + std::strlen(name));
    std::wstring w(256, L'\0');
    for (int attempt = 0; attempt < 4; ++attempt) {
        const DWORD n =
            ::GetEnvironmentVariableW(wname.c_str(), w.data(), static_cast<DWORD>(w.size()));
        if (n == 0) return {};
        if (n < w.size()) { w.resize(n); break; }
        w.assign(n, L'\0');  // n is the size needed, the terminator included: read again
        if (attempt == 3) return {};
    }
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), bytes,
                              nullptr, nullptr) != bytes)
        return {};
    return out;
}

std::string ReadScenario() {
    // The test-launch signal is the process-scoped VOTVCOOP_SCENARIO (set by the test launcher); a
    // native launch inherits none and boots to VOTV's own main menu, where the MULTIPLAYER button
    // drives coop. No on-disk fallback: a scenario file a launcher wrote into the game dir once
    // survived and auto-loaded the next native launch into gameplay; a per-launch mode needs a
    // per-launch signal.
    const std::string env = ReadEnv("VOTVCOOP_SCENARIO");
    return env.empty() ? "menu" : env;
}

// Trims the edges only: the value side keeps its interior spaces, since audio device names are
// matched by substring against the enumerated list, and a strip-all read mangled them into
// never-matching strings that silently fell back to the default device. Keys carry no spaces.
static std::string TrimEdges(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);  // not-name-text: ini whitespace trim
}

// Splits a line at its first '=' into an edge-trimmed key and an edge-trimmed value; false for
// a line without '=' or with an empty key (comments, blanks and section headers fall out).
static bool ParseIniLine(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = TrimEdges(line.substr(0, eq));
    value = TrimEdges(line.substr(eq + 1));  // not-name-text
    return !key.empty();
}

// One lock for every multivoid.ini access: writers come from two threads (the render thread's
// skins and voice panels, the game thread's boot default writes), and an unserialised
// read-modify-write pair let one writer rebuild the file from the other's half-written state.
// Readers take it too, so a read never sees the pre-rename transition.
static std::mutex g_iniMutex;

// Whether any live-ini access hit Unreadable this launch, and whether the minted identity is
// session-only (the mint gate, or a failed persist); the boot sweep reads them for its panel
// rows. Set for the module-dir ini only, never a selftest corpus path.
static std::atomic<bool> g_iniUnreadableSeen{false};
static std::atomic<bool> g_identityNotDurable{false};

// The one ini line primitive. Three fixed line buffers once served one file format, and a line
// longer than its consumer's buffer split, its tail parsing as a phantom key; one unbounded reader
// delivers whole lines to every consumer. The scan verdict is a tri-state: Ok is a clean end of
// stream (a caller's absent verdict is authoritative), Absent is ENOENT at open, Unreadable is any
// other open failure or a mid-stream read error (fgets conflates EOF with a stream error, and a
// mid-stream failure once read as absent for every key past it, so the writer rebuilt the file
// from the truncated prefix). The enum lives in config_internal.h for the write TU.
using IniScan = internal::IniScan;

// One line, unbounded: fgets chunks accumulate until the newline. True when a line is delivered,
// with its newline when the file carries one. A line a read error cut short is not delivered: its
// prefix could read as a different, valid value.
static bool ReadOneLine(FILE* f, std::string& out) {
    out.clear();
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) {
        out += buf;
        if (!out.empty() && out.back() == '\n') return true;
    }
    return !out.empty() && !std::ferror(f);  // final line without a trailing newline
}

// The line source seam (+1 a line, 0 a clean end, -1 a stream error): production wraps a FILE,
// and the config selftest injects a failing source to prove the error branch yields Unreadable,
// never Absent.
struct LineSource {
    int (*next)(void* ctx, std::string& out);
    void* ctx;
};

static int FileLineSourceNext(void* ctx, std::string& out) {
    FILE* f = static_cast<FILE*>(ctx);
    if (ReadOneLine(f, out)) return 1;
    return std::ferror(f) ? -1 : 0;
}

template <typename Fn>
static IniScan ScanLineSource(LineSource src, Fn&& cb) {
    std::string line;
    bool first = true;
    for (;;) {
        const int r = src.next(src.ctx, line);
        if (r < 0) return IniScan::Unreadable;
        if (r == 0) return IniScan::Ok;
        // An editor's UTF-8 byte-order mark belongs to the file, not to its first key, which it
        // would otherwise turn into an unknown one.
        if (first && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        first = false;
        cb(line);
    }
}

template <typename Fn>
static IniScan ScanIniFileAt(const std::wstring& path, Fn&& cb) {
    FILE* f = nullptr;
    const errno_t rc = _wfopen_s(&f, path.c_str(), L"r");
    if (rc != 0 || !f) return rc == ENOENT ? IniScan::Absent : IniScan::Unreadable;
    const IniScan st = ScanLineSource(LineSource{&FileLineSourceNext, f}, cb);
    std::fclose(f);
    return st;
}

static std::wstring IniPath() {
    const std::wstring dir = ue_wrap::paths::ExeDir();
    return dir.empty() ? std::wstring{} : dir + L"\\multivoid.ini";
}

static std::string StripInlineComment(const std::string& v, bool wsPrecededOnly);

// The seams the write TU uses (config_internal.h).
namespace internal {
std::mutex& IniMutex() { return g_iniMutex; }
std::wstring LiveIniPath() { return IniPath(); }
IniScan ScanIniFile(const std::wstring& path,
                    const std::function<void(const std::string&)>& cb) {
    return ScanIniFileAt(path, cb);
}
std::string TrimEdgesStr(const std::string& s) { return TrimEdges(s); }
bool ParseIniKeyValue(const std::string& line, std::string& key, std::string& value) {
    return ParseIniLine(line, key, value);
}
std::string StripInlineCommentStr(const std::string& v, bool wsPrecededOnly) {
    return StripInlineComment(v, wsPrecededOnly);
}
}  // namespace internal

// The occurrence rule: the authoritative line of a key is its first occurrence by
// case-insensitive key equality, one rule for the string layer, the flag layer, the writer and
// the sweep; the layers differ only in value vocabulary. (A case-sensitive first-key string read
// beside a first-recognised-value flag read silently swallowed garbage.)

// Inline-comment stripping, in the value layer. The string layer cuts at the first ';' preceded
// by whitespace (an interior ';' with no space before it stays, so device names round-trip); the
// flag layer cuts unconditionally, since flag lines carry inline comments and never a legitimate
// ';'.
static std::string StripInlineComment(const std::string& v, bool wsPrecededOnly) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != ';') continue;
        if (!wsPrecededOnly || i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')
            return TrimEdges(v.substr(0, i));  // not-name-text: an inline ini comment
    }
    return v;
}

// The one truthiness vocabulary, case-insensitive: 1, true, yes, on against 0, false, no, off.
// Anything else, an empty value included, is garbage: 0, the caller's default applies in memory,
// the sweep reports it, and nothing rewrites the file.
static int FlagVerdictFromValue(const std::string& raw) {
    std::string v = StripInlineComment(raw, /*wsPrecededOnly=*/false);
    for (char& c : v) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return 1;
    if (v == "0" || v == "false" || v == "no" || v == "off") return -1;
    return 0;
}

// Whole-string numeric parses: "1.25abc" and "" are garbage, not 1.25 and 0 (a prefix-accepting
// atof once turned voice.volume=abc into silence).
static bool ParseWholeLong(const std::string& s, long& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtol(s.c_str(), &end, 10);
    return end && *end == '\0';
}
static bool ParseWholeDouble(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

static bool EnumTokenMatch(const config_registry::Row* row, const std::string& v,
                           std::string& canonical) {
    if (!row || !row->tokens) return false;
    const char* t = row->tokens;
    while (*t) {
        const char* bar = std::strchr(t, '|');
        const size_t len = bar ? static_cast<size_t>(bar - t) : std::strlen(t);
        if (v.size() == len && _strnicmp(v.c_str(), t, len) == 0) {
            canonical.assign(t, len);  // registry spelling wins (all-lowercase)
            return true;
        }
        if (!bar) break;
        t = bar + 1;
    }
    return false;
}

// The one reader-equivalent validation (config.h), shared by the writer's refusal and the sweep:
// a value the sweep flags is a value the read rejects, by construction.
bool ValueValidForKey(const char* key, const std::string& rawValue, std::string* reasonOut) {
    const config_registry::Row* row = config_registry::FindRow(key);
    if (!row) return true;
    using config_registry::Kind;
    const std::string v = StripInlineComment(rawValue, /*wsPrecededOnly=*/false);
    char buf[128];
    switch (row->kind) {
        case Kind::Flag:
            if (FlagVerdictFromValue(v) != 0) return true;
            if (reasonOut) *reasonOut = "not 1/true/yes/on or 0/false/no/off";
            return false;
        case Kind::Int: {
            long n = 0;
            if (ParseWholeLong(v, n) && n >= static_cast<long>(row->lo) &&
                n <= static_cast<long>(row->hi))
                return true;
            std::snprintf(buf, sizeof(buf), "not a whole number in [%ld, %ld]",
                          static_cast<long>(row->lo), static_cast<long>(row->hi));
            if (reasonOut) *reasonOut = buf;
            return false;
        }
        case Kind::Float: {
            double d = 0;
            if (ParseWholeDouble(v, d) && d >= row->lo && d <= row->hi) return true;
            std::snprintf(buf, sizeof(buf), "not a number in [%g, %g]", row->lo, row->hi);
            if (reasonOut) *reasonOut = buf;
            return false;
        }
        case Kind::Enum: {
            std::string canonical;
            if (EnumTokenMatch(row, v, canonical)) return true;
            if (reasonOut) *reasonOut = std::string("not one of: ") + row->tokens;
            return false;
        }
        default:
            return true;
    }
}

// The reader core by path, unlocked (the public wrappers hold the lock; the selftest feeds corpus
// files). On Absent or Unreadable the caller still gets `def`, and the verdict reaches the
// callers that discriminate (the seeder, the mint gate, the sweep) through scanOut.
static std::string ReadIniValueAt(const std::wstring& path, const char* key,
                                  const char* def, IniScan* scanOut) {
    std::string result = def;
    bool found = false;
    const IniScan st = ScanIniFileAt(path, [&](const std::string& line) {
        if (found) return;
        std::string k, v;
        if (ParseIniLine(line, k, v) && _stricmp(k.c_str(), key) == 0) {
            result = StripInlineComment(v, /*wsPrecededOnly=*/true);
            found = true;
        }
    });
    if (scanOut) *scanOut = st;
    return result;
}

std::string ReadIniValue(const char* key, const char* def, IniScan* scanOut) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    IniScan st = IniScan::Ok;
    std::string v = ReadIniValueAt(IniPath(), key, def, &st);
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    if (scanOut) *scanOut = st;
    return v;
}

// The P2P transport fields of `c` from env, then ini, then default, for a session configured
// with no master (ReadNetConfig's p2p topology); a master lobby takes its rendezvous and ICE
// servers from the master's answer instead. The candidate policy, net.ice, is not a field: every
// session start reads it (Session::Start, coop/net/ice_policy.h).
static void FillP2PFields(coop::net::Config& c) {
    // The signaling rendezvous server; both peers connect outbound, no port forward. Only a session
    // dialled with no master reads this (a lobby's rendezvous and token come from its master's own
    // answer), and empty means the relay on the chosen master's host, which the P2P entry resolves
    // (coop/net/session_start.cpp): which master is chosen is the net layer's to say, not config's.
    c.signalingUrl = ResolveString(config_registry::rows::net_signaling);
    c.signalingToken = ResolveString(config_registry::rows::net_signaling_token);

    // This peer's own signaling identity is not configured: it is the install's durable public key
    // (peer_identity::LocalIdentityString), unique by construction, so the name collision the old
    // defaults worked around cannot arise.

    // The host identity a client dials, which must equal the host's own rendered identity; still
    // configurable for a dev dialling a host directly, from the `gen:` line in the host's log. No
    // fallback name: a dialled identity must parse as `gen:<64 hex>`, so a default could only ever
    // fail to parse and hide the real condition, that none was configured.
    c.hostIdentity = ResolveString(config_registry::rows::net_host_identity);

    // The ICE candidate sources: STUN defaults to a public server so a cross-NAT test works (a
    // same-machine test connects on host and LAN candidates regardless); TURN is off by default
    // (the master mints ephemeral credentials; static ini credentials are for dev).
    c.stunList = ResolveString(config_registry::rows::net_stun);
    c.turnList = ResolveString(config_registry::rows::net_turn);
    c.turnUser = ResolveString(config_registry::rows::net_turn_user);
    c.turnPass = ResolveString(config_registry::rows::net_turn_pass);

    // Both as the player wrote them: an empty relay prints as the master's it will be.
    UE_LOGI("config: P2P fields -- identity=<durable key> host='%s' signaling='%s' stun='%s'",
            c.hostIdentity.c_str(),
            c.signalingUrl.empty() ? "(the chosen master's)" : c.signalingUrl.c_str(),
            c.stunList.c_str());
}

coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    // Typed reads: the env name rides the registry row; garbage is the default plus a sweep row.
    const std::string role = ResolveEnum(config_registry::rows::net_role);
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    c.peerIp = ResolveString(config_registry::rows::net_peer);

    // The range [1, 65535] lives on the row; an out-of-range or partial parse keeps the compiled
    // default, so a rejected value never reaches the cast.
    c.port = static_cast<uint16_t>(ResolveInt(config_registry::rows::net_port));

    // net.topology: "lan" (the default, direct IP) or "p2p" (ICE).
    c.topology = ResolveEnum(config_registry::rows::net_topology) == "p2p"
                     ? coop::net::Topology::P2P
                     : coop::net::Topology::LanDirect;

    if (c.topology == coop::net::Topology::P2P) FillP2PFields(c);

    // The host identity is not a P2P-only fact. FillP2PFields sets it only on the P2P branch,
    // because its first job was routing (signaling rendezvouses on it, and a direct client dials an
    // address); its second job is the key the joiner binds the host to, without which the lobby
    // password proof is not emitted, so a direct or LAN joiner whose config named the host was
    // still unbound and a locked lobby refused every honest friend. Client only, and empty stays
    // empty: a joiner given only an address is unbound, and a locked host's refusal is then the
    // correct one.
    if (c.role == coop::net::Role::Client && c.hostIdentity.empty())
        c.hostIdentity = ResolveString(config_registry::rows::net_host_identity);

    // The lobby password for both roles, so an ini-configured host obeys the same lock the menu
    // flow sets (otherwise a dedicated or scripted host was silently open). On a host it is the
    // secret the session requires, gated on `locked` so a stale password does not lock a session
    // the player did not mean to lock; on a client it is the one to offer, and a host that wants
    // nothing ignores it.
    if (c.role == coop::net::Role::Host) {
        if (ResolveFlag(config_registry::rows::net_lobby_locked)) {
            c.lobbyPassword = ResolveString(config_registry::rows::net_lobby_password);
            // The same downgrade HostWithSave does: a lock with no secret cannot be enforced, and
            // locked=1 with an empty password produced a host that required a password while
            // announcing itself open, on the lane where nobody is at a screen to notice.
            if (c.lobbyPassword.empty())
                UE_LOGW("config: net.lobby_locked=1 but net.lobby_password is empty -- "
                        "hosting OPEN. A lock with no secret refuses everyone while the "
                        "browser shows an unlocked server.");
        }
    } else {
        // A client offers net.join_password, never net.lobby_password: the second is the secret
        // this player's own hosted sessions require, and reading it here made anyone who had ever
        // hosted a locked lobby offer that password to every locked host they reached.
        c.lobbyPassword = ResolveString(config_registry::rows::net_join_password);
    }

    return c;
}

std::wstring ReadNickname() {
    // The nickname default is the registry row's; the env name rides the row.
    std::string nick = ResolveString(config_registry::rows::net_nick);
    // The ini holds UTF-8, widened as UTF-8: a byte-at-a-time widen turned a UTF-8 name into
    // mojibake that the sanitiser stripped whole, the root of Cyrillic nicknames not working. Lossy
    // is right here and only here (the file is ours). The cap is in bytes because the wire's
    // nicklen is a uint8, and in characters because that is the display policy: two bounds, on
    // boundaries a raw resize would split.
    nick = coop::text::CapUtf8Bytes(std::move(nick), coop::text::kNickMaxBytes);
    return coop::text::CapCodepoints(
        coop::text::FromUtf8Lossy(nick.data(), nick.size()),
        coop::player_handshake::kNickMaxChars);
}

// The mint gate: the skin identity mints and persists only when the ini's answer is authoritative
// (a clean scan said absent or malformed, or the file is absent). On Unreadable (a share lock, a
// mid-stream error) it is session-only and nothing is written: reading a locked file as absent
// once minted and overwrote the real ini the moment the lock released. The boot panel shows an
// "identity not durable" row for either outcome.

// A key from the live ini with its scan verdict, under the lock.
static std::string ReadLiveIniWithScan(const char* key, IniScan& st) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    std::string v = ReadIniValueAt(IniPath(), key, "", &st);
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return v;
}

std::string ReadPlayerSkin() {
    // The persisted body-skin choice (multivoid.ini player_skin=). A new identity (an absent or
    // malformed key) rolls a random starter from the converter-skin list, filtered to the paks on
    // this install, the stock body when none; persisted at once, so the roll happens once per
    // identity.
    IniScan st = IniScan::Ok;
    std::string skin = ReadLiveIniWithScan("player_skin", st);
    if (!coop::skins::IsValidSkinName(skin)) {
        skin = coop::skins::PickRandomStarterSkin();
        if (st == IniScan::Unreadable) {
            g_identityNotDurable.store(true, std::memory_order_relaxed);
            UE_LOGW("config: player_skin unreadable (ini locked/failing) -> '%s' "
                    "SESSION-ONLY; mint gate refuses to write over an unreadable ini",
                    skin.c_str());
        } else {
            // The log says whether the persist happened; on a locked file it does not.
            const bool persisted = WriteIniValue(config_registry::rows::player_skin, skin.c_str());
            if (!persisted) g_identityNotDurable.store(true, std::memory_order_relaxed);
            UE_LOGI("config: player_skin absent/invalid -> random starter '%s' (%s)",
                    skin.c_str(),
                    persisted ? "persisted to multivoid.ini" : "SESSION-ONLY -- ini write failed");
        }
    }
    return skin;
}

bool IdentityNotDurable() { return g_identityNotDurable.load(std::memory_order_relaxed); }
bool IniUnreadableSeen()  { return g_iniUnreadableSeen.load(std::memory_order_relaxed); }

// The file operations the sweep and the owner reformat use.

int ListLiveIniLines(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    const IniScan st =
        ScanIniFileAt(IniPath(), [&](const std::string& line) { out.push_back(line); });
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return static_cast<int>(st);
}

// The boolean ini flags: the same occurrence rule as the string layer (the first
// case-insensitive key occurrence is authoritative), only the value vocabulary differs. A
// first-recognised-value scan once skipped garbage lines and flipped a flag when a line moved
// past a duplicate.

namespace {

// A flag from an ini by path: +1 true, -1 false, 0 absent or garbage (the caller's default
// applies; the sweep reports the garbage). Unlocked; the public wrappers hold the lock.
int LookupTriStateAt(const std::wstring& path, const char* key) {
    int verdict = 0;
    bool found = false;
    const IniScan st = ScanIniFileAt(path, [&](const std::string& line) {
        if (found) return;
        std::string k, v;
        if (ParseIniLine(line, k, v) && _stricmp(k.c_str(), key) == 0) {
            found = true;
            verdict = FlagVerdictFromValue(v);
        }
    });
    if (st == IniScan::Unreadable && path == IniPath())
        g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return verdict;
}

int LookupTriState(const char* key) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    return LookupTriStateAt(IniPath(), key);
}

}  // namespace

bool MasterEnabled() {
    // Absent or garbage defaults to enabled, so the granular switches decide; only an explicit
    // falsy forces every dev feature off.
    return ResolveFlag(config_registry::rows::enabled);
}

// The typed layered reads (config.h).

// The per-kind validate-and-default cores, in internal:: so the selftest TU shares the exact
// product semantics: one core for the live resolve and its instrument.
namespace internal {

// The layered raw-value pick (config_internal.h). The row comes from the caller's typed handle,
// so no lookup and no unregistered key; the census passes a row straight off the table, which is
// the one caller that has no handle and needs none.
bool PickRawLayered(const config_registry::Row* row, std::string& raw, bool* fromEnvOut,
                    IniScan* scanOut) {
    if (fromEnvOut) *fromEnvOut = false;
    if (scanOut) *scanOut = IniScan::Ok;
    if (row->envVar) {
        const std::string e = ReadEnv(row->envVar);
        if (!e.empty()) {
            raw = e;
            if (fromEnvOut) *fromEnvOut = true;
            return true;
        }
    }
    static const char* kAbsent = "\x01<absent>";
    const std::string v = ReadIniValue(row->key, kAbsent, scanOut);
    if (v == kAbsent) return false;
    raw = v;
    return true;
}

bool FlagFromRaw(const config_registry::Row* row, bool have, const std::string& raw) {
    if (!have) return row->defB;
    const int v = FlagVerdictFromValue(raw);
    return v == 0 ? row->defB : v > 0;
}
long IntFromRaw(const config_registry::Row* row, bool have, const std::string& raw) {
    if (!have) return row->defI;
    long v = 0;
    if (!ParseWholeLong(StripInlineComment(raw, false), v)) return row->defI;
    if (v < static_cast<long>(row->lo) || v > static_cast<long>(row->hi))
        return row->defI;  // out of range is garbage: the default, and the sweep reports it
    return v;
}
float FloatFromRaw(const config_registry::Row* row, bool have, const std::string& raw) {
    if (!have) return row->defF;
    double v = 0;
    if (!ParseWholeDouble(StripInlineComment(raw, false), v)) return row->defF;
    if (v < row->lo || v > row->hi) return row->defF;
    return static_cast<float>(v);
}
std::string EnumFromRaw(const config_registry::Row* row, bool have, const std::string& raw) {
    if (!have) return row->defS;
    std::string canonical;
    if (EnumTokenMatch(row, StripInlineComment(raw, false), canonical)) return canonical;
    return row->defS;
}

std::string ReadIniValueAtPath(const std::wstring& path, const char* key, const char* def,
                               IniScan* scanOut) {
    return ReadIniValueAt(path, key, def, scanOut);
}

int LookupTriStateAtPath(const std::wstring& path, const char* key) {
    return LookupTriStateAt(path, key);
}

namespace {
struct FailingSourceCtx { int remaining; };
int FailingSourceNext(void* ctx, std::string& out) {
    auto* c = static_cast<FailingSourceCtx*>(ctx);
    if (c->remaining <= 0) return -1;   // injected mid-stream error
    --c->remaining;
    out = "injected_key=1\n";
    return 1;
}
}  // namespace

int ScanWithInjectedFailure(int failAfterLines) {
    FailingSourceCtx ctx{failAfterLines};
    const IniScan st =
        ScanLineSource(LineSource{&FailingSourceNext, &ctx}, [](const std::string&) {});
    // The branch under test: a mid-stream error must yield Unreadable, never a clean Ok that reads
    // as absent downstream.
    return static_cast<int>(st);
}

}  // namespace internal

bool ResolveFlag(const config_registry::FlagRow& h) {
    std::string raw;
    const bool have = internal::PickRawLayered(h.row, raw);
    return internal::FlagFromRaw(h.row, have, raw);
}

long ResolveInt(const config_registry::IntRow& h) {
    std::string raw;
    const bool have = internal::PickRawLayered(h.row, raw);
    return internal::IntFromRaw(h.row, have, raw);
}

float ResolveFloat(const config_registry::FloatRow& h) {
    std::string raw;
    const bool have = internal::PickRawLayered(h.row, raw);
    return internal::FloatFromRaw(h.row, have, raw);
}

std::string ResolveEnum(const config_registry::EnumRow& h) {
    std::string raw;
    const bool have = internal::PickRawLayered(h.row, raw);
    return internal::EnumFromRaw(h.row, have, raw);
}

FailClosedRead ResolveFailClosed(const config_registry::FailClosedEnumRow& h, std::string& out,
                                 std::string* refusedOut, std::string* originOut) {
    std::string raw;
    bool fromEnv = false;
    IniScan scan = IniScan::Ok;
    const bool have = internal::PickRawLayered(h.row, raw, &fromEnv, &scan);
    out.clear();
    // An ini that could not be read has no answer to give, and "absent" is not one: the default
    // would stand in for a line that may well say otherwise.
    if (!have && scan == IniScan::Unreadable) {
        if (originOut) *originOut = "multivoid.ini";
        return FailClosedRead::Unreadable;
    }
    // The one verdict the census, the writer and the boot sweep use, so a value refused here is a
    // value they report as refused.
    if (have && !ValueValidForKey(h.row->key, raw, nullptr)) {
        if (refusedOut) *refusedOut = raw;
        if (originOut) *originOut = fromEnv ? h.row->envVar : "multivoid.ini";
        return FailClosedRead::Refused;
    }
    out = internal::EnumFromRaw(h.row, have, raw);
    return FailClosedRead::Value;
}

std::string ResolveString(const config_registry::StringRow& h) {
    std::string raw;
    if (!internal::PickRawLayered(h.row, raw)) return h.row->defS;
    return raw;
}

}  // namespace coop::config
