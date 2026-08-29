// coop/config/config.cpp -- env + ini configuration readers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).

#include "coop/config/config.h"

#include "config_internal.h"
#include "ue_wrap/core/paths.h"
#include "coop/session/player_handshake.h"  // kNickMaxChars (ONE owner)
#include "coop/text/utf8_codec.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/skin_registry.h"  // IsValidSkinName + kDefaultSkinName (v93 player_skin=)
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
    // ARC D: read the environment WIDE and re-encode to UTF-8. Windows keeps the
    // environment block as UTF-16; GetEnvironmentVariableA converts it down to the
    // process ANSI codepage, so a Cyrillic VOTVCOOP_NET_NICK arrived here as cp1251
    // bytes -- not UTF-8, not well-formed UTF-8 either -- and every layer above,
    // which correctly assumes UTF-8, then produced a row of U+FFFD. Measured
    // 2026-07-28: the drill's names rendered as the placeholder while the ini path
    // beside it was already correct, which is a confusing way to find this.
    //
    // The rest of the config stack speaks UTF-8 (the ini is UTF-8, the browser
    // writes UTF-8), so this is the ONE place the boundary belongs.
    wchar_t wbuf[256] = {};
    const DWORD n = ::GetEnvironmentVariableW(
        std::wstring(name, name + std::strlen(name)).c_str(), wbuf,
        static_cast<DWORD>(std::size(wbuf)));
    if (n == 0 || n >= std::size(wbuf)) return {};
    return coop::text::ToUtf8(std::wstring(wbuf, wbuf + n));
}

std::string ReadScenario() {
    // The TEST-launch signal is the PROCESS-SCOPED env var VOTVCOOP_SCENARIO
    // (set by mp.py / play-coop.bat / lan-test). A NATIVE launch (double-click /
    // Steam) inherits no such env -> it falls through to "menu": boot to VOTV's
    // own main menu, where the MULTIPLAYER button (server browser + Host-Game
    // save picker) drives coop. NO auto-load into gameplay on a native launch.
    //
    // RETIRED (2026-06-06, RULE 1 root cause / RULE 2 no leak-prone parallel
    // mechanism): the old on-disk `scenario.txt` fallback. A test launcher wrote
    // scenario.txt="play" INTO THE GAME DIR, and it survived on disk -- so the
    // NEXT native VotV.exe launch read the leftover file and auto-loaded straight
    // into gameplay (user-reported 2026-06-06). A per-launch mode MUST use a
    // per-launch signal (env), never a file that aliases later native launches.
    const std::string env = ReadEnv("VOTVCOOP_SCENARIO");
    return env.empty() ? "menu" : env;
}

// Trim leading/trailing whitespace (space, tab, CR, LF). The VALUE side of a key=value
// line keeps its INTERIOR spaces verbatim: audio device names ("Voicemeeter Out B1
// (VB-Audio ...)") are matched by substring against the enumerated device list, and the
// old strip-ALL-whitespace read mangled them into never-matching strings, silently
// falling back to the default device (the 2026-06-12 voice-inaudible root cause #1).
// Keys themselves never contain spaces, so edge-trimmed key equality is exact.
static std::string TrimEdges(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);  // not-name-text: ini whitespace trim
}

// Split a raw ini line at the first '=' into an edge-trimmed key and an edge-trimmed,
// interior-verbatim value. False for lines without '=' or with an empty key (comments,
// blanks, section headers fall out naturally -- '#'/';' never equals a real key).
static bool ParseIniLine(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    // not-name-text: splitting an ini line at its '='
    key = TrimEdges(line.substr(0, eq));
    value = TrimEdges(line.substr(eq + 1));  // not-name-text
    return !key.empty();
}

// One lock for every multivoid.ini access in this process. Writers come from TWO
// threads (render: skins-panel RequestSkin / voice-panel device save; game: boot
// default-writes) -- an unserialized read-modify-write pair can interleave and one
// writer rebuilds the file from the other's half-written state. Readers take it too
// so a read never observes the (pre-atomic-rename) transition. (Until 2026-07-25
// the FLAG reader did NOT take it -- this comment was false; the arc-1 primitive
// funnels every consumer through the lock-holding public API.)
static std::mutex g_iniMutex;

// T10 state: did any LIVE-ini access hit UNREADABLE this launch, and is the
// minted identity session-only (mint gate / failed persist)? Set only for the
// module-dir ini -- selftest corpus paths never touch these. The boot sweep
// reads them for the "identity not durable" / "ini unreadable" panel rows.
static std::atomic<bool> g_iniUnreadableSeen{false};
static std::atomic<bool> g_identityNotDurable{false};

// ---- the ONE ini line primitive (config rework arc 1, 2026-07-25) ------------
// THREE fixed line buffers used to ship for one file format (128 flag / 256
// reader / 512 writer chunk). A line longer than its consumer's buffer SPLIT,
// and a tail chunk whose bytes parsed as "key=..." became a PHANTOM KEY (live
// in 2 of the 4 dev-rig inis; design F8/F31). One UNBOUNDED reader retires the
// class: every consumer sees whole lines, verbatim (trailing newline kept).
//
// The scan verdict is a TRI-STATE (design F37/F38):
//   Ok         -- clean end of stream (feof, no ferror): a caller's ABSENT
//                 verdict is authoritative;
//   Absent     -- the file does not exist (ENOENT at open);
//   Unreadable -- open failed for any OTHER reason (share lock, perms), or a
//                 MID-STREAM read error. fgets' NULL conflates EOF with stream
//                 error; before this, a mid-stream failure read as "absent" for
//                 every key past it, and the writer rebuilt the file from the
//                 truncated prefix (the 2026-07-02 loss class, one layer deeper).
// The enum lives in config_internal.h since the C6 extraction (the write TU
// shares it).
using IniScan = internal::IniScan;

// One line, unbounded: accumulate fgets chunks until the newline arrives.
// True = a line is delivered (verbatim, incl. '\n' when the file carries one).
static bool ReadOneLine(FILE* f, std::string& out) {
    out.clear();
    char buf[512];
    while (std::fgets(buf, sizeof(buf), f)) {
        out += buf;
        if (!out.empty() && out.back() == '\n') return true;
    }
    return !out.empty();  // final line without a trailing newline
}

// The line SOURCE seam: +1 = line delivered, 0 = clean end, -1 = stream error.
// Production wraps FILE*; the config selftest injects a failing source to prove
// the ferror branch returns Unreadable, never Absent (design T4 fault injection).
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
    for (;;) {
        const int r = src.next(src.ctx, line);
        if (r < 0) return IniScan::Unreadable;
        if (r == 0) return IniScan::Ok;
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

// ---- the TU-private seams for the write TU (config_internal.h) --------------
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

// ---- the unified occurrence rule + the two value layers (arc 2, T4/T5) ------
// The authoritative line of key K = the FIRST occurrence of K by
// CASE-INSENSITIVE key equality. ONE rule shared by the string layer, the
// flag layer, the writer and the T10 sweep; the layers differ ONLY in value
// vocabulary, never in line selection. The old pair -- case-sensitive
// first-KEY (string) vs first-RECOGNIZED-VALUE (flag, the F25 legacy that
// silently swallowed garbage) -- is dead. Safety measured: F40 (zero ci
// collisions between distinct keys, zero case twins in the rig).

// T5: inline-comment stripping lives here, in the lexer's value layer.
// String layer: cut at the first ';' PRECEDED by whitespace (F18 narrowing --
// an interior ';' with no space before it stays part of the value: device
// names round-trip verbatim). Flag layer: unconditional cut (flag lines carry
// inline `; comments` pervasively and never a legitimate ';').
static std::string StripInlineComment(const std::string& v, bool wsPrecededOnly) {
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] != ';') continue;
        if (!wsPrecededOnly || i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')
            return TrimEdges(v.substr(0, i));  // not-name-text: an inline ini comment
    }
    return v;
}

// The ONE truthiness vocabulary (T6): 1|true|yes|on / 0|false|no|off, ci.
// Anything else -- including present-but-empty (F33) -- is garbage: 0, and the
// caller's default applies in memory ("значение говно и несуразица -> ставится
// дефолт"); the T10 sweep reports it, nothing rewrites the file.
static int FlagVerdictFromValue(const std::string& raw) {
    std::string v = StripInlineComment(raw, /*wsPrecededOnly=*/false);
    for (char& c : v) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return 1;
    if (v == "0" || v == "false" || v == "no" || v == "off") return -1;
    return 0;
}

// Whole-string numeric parses: "1.25abc" and "" are garbage, not 1.25/0.
// (The old per-site atof/strtol accepted any prefix and silently produced 0
// from pure garbage -- voice.volume=abc used to mean SILENCE.)
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

// The ONE reader-equivalent validation (see config.h). Shared by the T3b
// writer refusal and the T10 sweep -- a value the sweep flags is exactly a
// value the read rejects, by construction.
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

// Path-parameterized reader core (no lock -- public wrappers hold it; the
// selftest feeds corpus files). On Absent/Unreadable the caller still
// receives `def` -- the tri-state reaches discriminating callers (seeder,
// mint gate, sweep) via scanOut.
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

std::string ReadIniValue(const char* key, const char* def) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    IniScan st = IniScan::Ok;
    std::string v = ReadIniValueAt(IniPath(), key, def, &st);
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return v;
}

// ---- Built-in (hardcoded) public net endpoints -- our VPS -----------------------
// A fresh install with NO multivoid.ini reaches these out of the box: the menu server
// browser + Host-Game flow hit the real master, which then mints the per-session
// signaling token + STUN + ephemeral TURN creds. These are PUBLIC connection endpoints
// (the master IP:port is advertised to every client), NOT secrets -- the signaling
// TOKEN, the TURN secret, and the SSH/ops creds are deliberately NOT compiled in (they
// stay in the local-only ini, or the master mints them per session; only net.master is
// strictly required for the normal flow). The `net.master.custom=1` ini gate opts OUT
// of these and uses the ini's own net.master / net.signaling (run-your-own-master).
// The constants live in coop/net/protocol.h (kOfficial*Url) -- shared with the UI
// display mask that prints "DEFAULT" instead of the raw VPS address. (Arc 3: the
// old file-static kBuiltin* alias copies are DELETED -- the registry rows for
// net.master / net.signaling alias the SAME owning constants, and this TU uses
// them directly.)

// The custom-master gate. net.master.custom = 1/true/yes/on opts out of the hardcoded
// VPS endpoints and uses the ini's net.master / net.signaling instead. Default OFF ->
// the built-in VPS endpoints win (a stale net.master in the ini is ignored unless the
// gate is set), which is what makes a no-config native install Just Work. An env
// override (VOTVCOOP_MASTER_URL / VOTVCOOP_NET_SIGNALING) always takes precedence over
// both (the dev / LAN-test framework).
static bool UseCustomNetMaster() {
    return ResolveFlag(config_registry::rows::net_master_custom);
}

// Fill the P2P (rungs 1-3) transport fields of `c` from env -> ini -> default.
// Shared by ReadNetConfig (when net.topology=p2p) AND ReadP2PHostFallback (the
// menu Host-Game master-unreachable fallback), so the env/ini key set lives in
// ONE place (RULE 2). Uses c.role to pick the identity default (host vs client).
static void FillP2PFields(coop::net::Config& c) {
    // Signaling rendezvous server. Both peers connect OUTBOUND -- no host port-forward.
    // Precedence mirrors the master: env -> custom-master gate (ini net.signaling) ->
    // the built-in VPS signaling. (The signaling TOKEN stays ini/master-minted -- never
    // hardcoded; in the normal master-up flow the master overrides this URL+token per
    // session, so this default only seeds the master-down fallback.) The gate makes
    // this chain bespoke (env always wins, ini only under the gate -- ResolveString
    // cannot express the skipped middle layer), but the env NAME rides the row (arc 3
    // T2b: no site-owned "VOTVCOOP_NET_SIGNALING" literal twin).
    std::string sig = ReadEnv(config_registry::rows::net_signaling.row->envVar);
    if (sig.empty())
        sig = UseCustomNetMaster()
                  ? ResolveString(config_registry::rows::net_signaling)
                  : std::string(coop::net::kOfficialSignalingUrl);
    c.signalingUrl = sig;
    c.signalingToken = ResolveString(config_registry::rows::net_signaling_token);

    // This peer's own signaling identity is NOT configured any more (RULE 2,
    // 2026-08-29). It is the install's durable public key, rendered by
    // peer_identity::LocalIdentityString(). The `net.identity` row and its
    // "votvhost" / "votvclient-XXXX" defaults are retired with it -- their whole
    // purpose was to hand each peer a name UNIQUE on the signaling server, and a
    // keypair is unique by construction, so the collision the defaults worked
    // around cannot arise from an un-configured install.

    // The host identity a client dials (must equal the host's own rendered
    // identity). Still configurable: a dev dialling a host directly, with no
    // master in the loop, copies the `gen:` line out of the host's log.
    std::string hostId = ResolveString(config_registry::rows::net_host_identity);
    if (hostId.empty()) hostId = "votvhost";
    c.hostIdentity = hostId;

    // ICE candidate sources. STUN (rung 2) defaults to a public server so a
    // real cross-NAT test works; for a same-machine test ICE also connects
    // via host/LAN candidates regardless. TURN (rung 3) is off by default
    // (the master mints ephemeral REST creds; static ini creds are dev-only).
    c.stunList = ResolveString(config_registry::rows::net_stun);
    c.turnList = ResolveString(config_registry::rows::net_turn);
    c.turnUser = ResolveString(config_registry::rows::net_turn_user);
    c.turnPass = ResolveString(config_registry::rows::net_turn_pass);

    // ICE candidate policy: "" / "all" (default) / "relay" / "disable" /
    // "default". "relay" forces the TURN relay path (privacy, or to validate
    // coturn end-to-end). Mapped to IceEnable in Session::StartP2P. Enum row
    // (arc 2): env rides the row; an unknown token is garbage -> "" (default
    // policy) + a T10 sweep row.
    c.iceMode = ResolveEnum(config_registry::rows::net_ice);

    // Console-visible diagnostic: any endpoint on the OFFICIAL VPS host prints
    // as "DEFAULT" -- the connect console must not advertise the raw address
    // (the session_manager DisplayMaster twin; user 2026-06-10). A custom
    // endpoint prints verbatim (its operator debugs with it).
    auto maskOfficial = [](const std::string& v) -> std::string {
        std::string host = coop::net::kOfficialMasterUrl;
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host.resize(colon);  // not-name-text: host:port
        return v.rfind(host, 0) == 0 ? std::string("DEFAULT") : v;
    };
    UE_LOGI("config: P2P fields -- identity=<durable key> host='%s' signaling='%s' stun='%s'",
            c.hostIdentity.c_str(),
            maskOfficial(c.signalingUrl).c_str(), maskOfficial(c.stunList).c_str());
}

coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    // Typed reads (arc 2): env rides the registry row (VOTVCOOP_NET_ROLE /
    // _PORT / _TOPOLOGY); garbage -> the default + a T10 sweep row.
    const std::string role = ResolveEnum(config_registry::rows::net_role);
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    c.peerIp = ResolveString(config_registry::rows::net_peer);

    // Range [1,65535] lives on the registry row; out-of-range or a partial
    // parse is garbage -> the compiled default stays (the old strtoul-wrap
    // hazard is structurally gone: a rejected value never reaches the cast).
    c.port = static_cast<uint16_t>(ResolveInt(config_registry::rows::net_port));

    // --- P2P (zero-open-ports) topology --------------------------------------
    // net.topology = "lan" (default, rung 0/1 IP) or "p2p" (rungs 1-3 ICE).
    c.topology = ResolveEnum(config_registry::rows::net_topology) == "p2p"
                     ? coop::net::Topology::P2P
                     : coop::net::Topology::LanDirect;

    if (c.topology == coop::net::Topology::P2P) FillP2PFields(c);

    return c;
}

std::string ReadMasterUrl() {
    // Master/lobby server "host:port". Precedence: env (LAN-test framework) -> the
    // custom-master gate (net.master.custom=1 -> ini net.master) -> the BUILT-IN VPS
    // endpoint. A native launch has no env and (by default) no custom gate, so it hits
    // the hardcoded VPS master, which drives the menu server browser + Host-Game flow
    // and mints the per-session signaling/STUN/TURN creds.
    // Env name from the row (arc 3 T2b) -- the gate makes the chain bespoke
    // (env beats both layers; ini counts only under the gate), the literal
    // does not duplicate.
    std::string m = ReadEnv(config_registry::rows::net_master.row->envVar);
    if (!m.empty()) return m;
    if (UseCustomNetMaster()) {
        std::string v = ResolveString(config_registry::rows::net_master);
        // "DEFAULT" sentinel (the shipped release ini): resolves to the official
        // server even under the custom gate -- the ini never needs the raw VPS
        // address spelled out.
        std::string lower = v;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (v.empty() || lower == "default") return coop::net::kOfficialMasterUrl;
        return v;
    }
    return coop::net::kOfficialMasterUrl;
}

coop::net::Config ReadP2PHostFallback() {
    // The transport Config the menu Host-Game flow uses when the master announce
    // FAILS (master down) -- so hosting NEVER silently dies on an unreachable
    // master (RULE 1 decouple). Forced P2P host; signaling/identity/stun come
    // from the same env/ini keys as the normal P2P path (the deployed ini points
    // these at the VPS). Unlisted, but the host still boots + a configured peer
    // can still join. MTA precedent: the server runs regardless of the master list.
    coop::net::Config c;
    c.role = coop::net::Role::Host;
    c.topology = coop::net::Topology::P2P;
    FillP2PFields(c);
    return c;
}

std::wstring ReadNickname() {
    // T7 (ini rework): the MY-NAME default is the registry row's (env rides the
    // row's VOTVCOOP_NET_NICK; never a per-site literal -- design F19/T2-migrate).
    std::string nick = ResolveString(config_registry::rows::net_nick);
    // ARC D: the ini holds UTF-8. This used to be
    //     std::wstring(nick.begin(), nick.end())
    // which widens ONE BYTE AT A TIME -- a Latin-1 widen. A UTF-8 name arrived at
    // SanitizeNickname as N mojibake wchars and was stripped whole, which is the
    // MEASURED root of "Cyrillic nicknames do not work". Lossy is right here and
    // only here: the file is ours, not a stranger's.
    //
    // The cap is in BYTES because the wire's nicklen is uint8 (F14) and in
    // CHARACTERS because that is the display policy -- two different questions,
    // so two different bounds, applied on boundaries a raw resize() would split.
    nick = coop::text::CapUtf8Bytes(std::move(nick), coop::text::kNickMaxBytes);
    return coop::text::CapCodepoints(
        coop::text::FromUtf8Lossy(nick.data(), nick.size()),
        coop::player_handshake::kNickMaxChars);
}

// MINT GATE (arc 2, T6/F43): both computed identities mint-and-persist ONLY
// when the ini's answer is AUTHORITATIVE (a clean scan said absent/malformed,
// or the file itself is absent). On UNREADABLE (share lock, mid-stream read
// error) the identity is SESSION-ONLY and WriteIniValue is never called --
// the old path read a locked file as "absent", minted, and OVERWROTE the real
// ini the moment the lock released (the lock-release overwrite race).
// The T10 panel shows the "identity not durable" row for either outcome.

// Reads a key from the LIVE ini with its scan verdict (locked wrapper).
static std::string ReadLiveIniWithScan(const char* key, IniScan& st) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    std::string v = ReadIniValueAt(IniPath(), key, "", &st);
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return v;
}

std::string ReadPlayerSkin() {
    // v93 skins: the persisted body-skin choice, stored NEXT TO the player identity
    // (multivoid.ini "player_skin=", same file as player_guid -- user 2026-07-02).
    // v95: a NEW identity (absent/malformed key) rolls a RANDOM starter from the
    // curated converter-skin list (user: "для НОВЫХ пиров случайный скин из списка"),
    // filtered to paks present on this install -- hl_einstein_v1sc when none is.
    // Persisted immediately (like the guid), so the roll happens ONCE per identity.
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
            // Truthful persist log (F43 fix): the old line said "persisted"
            // unconditionally -- false on a locked file.
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

// ---- T10 sweep / T1b owner-reformat file operations (arc 2) -----------------

int ListLiveIniLines(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    const IniScan st =
        ScanIniFileAt(IniPath(), [&](const std::string& line) { out.push_back(line); });
    if (st == IniScan::Unreadable) g_iniUnreadableSeen.store(true, std::memory_order_relaxed);
    return static_cast<int>(st);
}

// ---- boolean ini flags (merged from coop/session/ini_config, 2026-07-10) ----
// Since arc 2 (T4) the flag layer shares the unified occurrence rule with the
// string layer: the FIRST case-insensitive key occurrence is authoritative,
// and only the value VOCABULARY differs (FlagVerdictFromValue). The old
// first-RECOGNIZED-value scan (F25) -- which skipped garbage lines and could
// flip a flag when a line MOVED past a duplicate (F32) -- is retired.

namespace {

// Scan an ini for `key=...`:  +1 = true,  -1 = false,  0 = absent OR garbage
// value (callers' defaults apply; the T10 sweep reports the garbage). Lines
// are unbounded via the primitive; no lock here -- public wrappers hold
// g_iniMutex.
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
    // ABSENT/garbage defaults to enabled (= granular switches decide) -- the
    // row's def=true carries the old `LookupTriState != -1` semantics exactly
    // (measured outcome tables, arc-3 impl design R2). Only an explicit falsy
    // forces all dev features off.
    return ResolveFlag(config_registry::rows::enabled);
}

// ---- typed layered reads (arc 2, T6 -- see config.h) ------------------------

namespace {

// The layered raw-value pick: SET env wins (valid or not -- garbage env
// SHADOWS the ini, T6); else the ini's authoritative line; else absent.
// Returns true + `raw` when a layer supplied a value. Arc 3: the row comes
// from the caller's typed handle -- no lookup, no unregistered keys.
bool PickRawLayered(const config_registry::Row* row, std::string& raw) {
    if (row->envVar) {
        const std::string e = ReadEnv(row->envVar);
        if (!e.empty()) { raw = e; return true; }
    }
    static const char* kAbsent = "\x01<absent>";
    const std::string v = ReadIniValue(row->key, kAbsent);
    if (v == kAbsent) return false;
    raw = v;
    return true;
}

}  // namespace

// ---- the per-kind validate+default cores + the selftest-TU seams ------------
// internal:: so the selftest TU (config_selftest.cpp, the arc-3 soft-cap cut)
// shares the EXACT product semantics -- ONE core for live resolve and
// instrument twin, never two (C5).
namespace internal {

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
        return row->defI;  // out of range = garbage -> default (user ruling), sweep reports
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
    // The branch under test: a mid-stream error must yield Unreadable (2),
    // never a clean Ok that would read as ABSENT downstream (design F38).
    return static_cast<int>(st);
}

}  // namespace internal

bool ResolveFlag(const config_registry::FlagRow& h) {
    std::string raw;
    const bool have = PickRawLayered(h.row, raw);
    return internal::FlagFromRaw(h.row, have, raw);
}

long ResolveInt(const config_registry::IntRow& h) {
    std::string raw;
    const bool have = PickRawLayered(h.row, raw);
    return internal::IntFromRaw(h.row, have, raw);
}

float ResolveFloat(const config_registry::FloatRow& h) {
    std::string raw;
    const bool have = PickRawLayered(h.row, raw);
    return internal::FloatFromRaw(h.row, have, raw);
}

std::string ResolveEnum(const config_registry::EnumRow& h) {
    std::string raw;
    const bool have = PickRawLayered(h.row, raw);
    return internal::EnumFromRaw(h.row, have, raw);
}

std::string ResolveString(const config_registry::StringRow& h) {
    std::string raw;
    if (!PickRawLayered(h.row, raw)) return h.row->defS;
    return raw;
}

}  // namespace coop::config
