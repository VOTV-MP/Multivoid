// coop/config/config.cpp -- env + ini configuration readers.
//
// Extracted from harness/harness.cpp (2026-05-25 modular refactor).

#include "coop/config/config.h"

#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/skin_registry.h"  // IsValidSkinName + kDefaultSkinName (v93 player_skin=)
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace coop::config {

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    const size_t sep = p.find_last_of(L"\\/");
    return sep == std::wstring::npos ? L"." : p.substr(0, sep);
}

std::string ReadEnv(const char* name) {
    char buf[256] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf) : std::string();
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
    return s.substr(b, e - b + 1);
}

// Split a raw ini line at the first '=' into an edge-trimmed key and an edge-trimmed,
// interior-verbatim value. False for lines without '=' or with an empty key (comments,
// blanks, section headers fall out naturally -- '#'/';' never equals a real key).
static bool ParseIniLine(const std::string& line, std::string& key, std::string& value) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    key = TrimEdges(line.substr(0, eq));
    value = TrimEdges(line.substr(eq + 1));
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
enum class IniScan { Ok = 0, Absent = 1, Unreadable = 2 };

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

static std::wstring IniPath() { return ModuleDir() + L"\\multivoid.ini"; }

// Path-parameterized reader core (no lock -- public wrappers hold it; the
// selftest feeds corpus files). Match semantics UNCHANGED in arc 1: first
// case-SENSITIVE key occurrence (the unified ci rule lands with the arc-2
// layer flip). On Absent/Unreadable the caller still receives `def` -- the
// tri-state reaches discriminating callers (seeder, arc-2 mint gate) via out.
static std::string ReadIniValueAt(const std::wstring& path, const char* key,
                                  const char* def, IniScan* scanOut) {
    std::string result = def;
    bool found = false;
    const IniScan st = ScanIniFileAt(path, [&](const std::string& line) {
        if (found) return;
        std::string k, v;
        if (ParseIniLine(line, k, v) && k == key) { result = v; found = true; }
    });
    if (scanOut) *scanOut = st;
    return result;
}

std::string ReadIniValue(const char* key, const char* def) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    return ReadIniValueAt(IniPath(), key, def, nullptr);
}

bool EnsureIniSkeleton() {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    const std::wstring path = IniPath();
    // Seed ONLY on authoritative ABSENT (ENOENT). An existing file -- readable
    // or not -- is never touched: seeding over a locked-but-present ini is the
    // same destruction class the F7 writer guards close (design T1/F37).
    {
        FILE* probe = nullptr;
        const errno_t rc = _wfopen_s(&probe, path.c_str(), L"r");
        if (rc == 0 && probe) { std::fclose(probe); return false; }  // exists
        if (rc != ENOENT) {
            UE_LOGW("config: skeleton seeder skipped -- multivoid.ini unreadable (errno=%d), "
                    "not absent; refusing to seed over it", static_cast<int>(rc));
            return false;
        }
    }
    // The skeleton: ordered section headers from the registry ([net] first,
    // [dev] last) and ZERO default values (F4: a seeded key silently OVERRIDES
    // the code default) -- with exactly ONE user-ruled exception: a visible,
    // deliberately-editable net.nick line (the joke is meant to be SEEN and
    // replaced; design T1 "seeded-active").
    std::string content = "; multivoid.ini -- Multivoid configuration. Created on first launch.\n";
    for (size_t i = 0; i < coop::config_registry::kSectionCount; ++i) {
        const char* sec = coop::config_registry::kSectionOrder[i];
        content += "\n[";
        content += sec;
        content += "]\n";
        if (std::string(sec) == "net")
            content += std::string("net.nick=") + coop::config_registry::kMyNameDefault + "\n";
    }
    // Atomic create: .new then MoveFileExW WITHOUT REPLACE_EXISTING -- if the
    // file appeared concurrently the seeder loses the race gracefully.
    const std::wstring tmp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") != 0 || !f) {
        UE_LOGW("config: skeleton seeder could not open multivoid.ini.new for write");
        return false;
    }
    bool wrote = std::fputs(content.c_str(), f) != EOF;
    if (std::ferror(f)) wrote = false;
    if (std::fclose(f) != 0) wrote = false;
    if (!wrote) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: skeleton seeder write FAILED (disk?) -- no ini created");
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: skeleton seeder lost the create race (err=%lu) -- existing ini kept",
                ::GetLastError());
        return false;
    }
    UE_LOGI("config: seeded fresh multivoid.ini skeleton ([net] first, net.nick=%s, [dev] last)",
            coop::config_registry::kMyNameDefault);
    return true;
}

// Path-parameterized writer core (no lock -- the public wrapper holds it; the
// selftest drives COPIES of corpus files, never the live ini).
static bool WriteIniValueAt(const std::wstring& path, const char* key, const char* value) {
    // Scrub CR/LF from the value (an embedded newline -- e.g. pasted into a text field --
    // would split the "key=value" line and corrupt the NEXT key on read-back), then
    // edge-trim. Interior spaces are part of the value (device names) and round-trip
    // verbatim through ReadIniValue's parse.
    std::string safe;
    for (const char* p = value; *p; ++p)
        if (*p != '\n' && *p != '\r') safe.push_back(*p);
    safe = TrimEdges(safe);
    const std::string newLine = std::string(key) + "=" + safe + "\n";
    // Read existing lines, replacing the key's line IN PLACE if present (so we keep
    // the rest of the ini -- sections, comments, other keys -- untouched).
    //
    // DESTRUCTION GUARDS (born 2026-07-02: the HOST's ini lost everything above its
    // last-appended keys -- [dev] header, devkeys=1, enabled=1 -- and the F1 dev menu
    // silently vanished; the file had been rebuilt from appends after an obliteration):
    //   1. if the ini EXISTS but cannot be opened for read (editor/AV/backup holding a
    //      lock), ABORT the write -- the old code carried on with an EMPTY line list
    //      and truncated the whole file down to the one new key;
    //   2. the new content goes to multivoid.ini.new, then MoveFileExW REPLACE_EXISTING
    //      swaps it in ATOMICALLY -- the old truncate-then-write left a window (crash,
    //      kill, power) where the file on disk was empty/partial.
    std::vector<std::string> lines;
    bool found = false;
    {
        FILE* f = nullptr;
        errno_t rc = 1;
        for (int attempt = 0; attempt < 5; ++attempt) {   // transient sharing locks
            rc = _wfopen_s(&f, path.c_str(), L"r");
            if (rc == 0 && f) break;
            // Existence re-checked PER ATTEMPT (not a pre-loop snapshot): an ini
            // created between a stale snapshot and a transiently-failing open must
            // not take the "fresh file" path and get rebuilt down to one key.
            if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
            ::Sleep(20);
        }
        if ((rc != 0 || !f) &&
            ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            UE_LOGW("config: WriteIniValue('%s') SKIPPED -- multivoid.ini exists but is "
                    "locked for read; refusing to rebuild the file blind", key);
            return false;
        }
        if (rc == 0 && f) {
            // Whole lines via the arc-1 primitive: the old 512-byte chunk loop
            // ran ParseIniLine per CHUNK, so a long line whose tail parsed as
            // the key under write got spliced (design F31). Dead class now.
            //
            // TARGETING = the unified occurrence rule (T3): the authoritative
            // line is the FIRST case-INSENSITIVE key occurrence, edited in
            // place with the canonical spelling (newLine carries the caller's
            // key -- safe: zero ci collisions between distinct keys, F40). The
            // old case-sensitive writer MISSED `Enabled=1` when writing
            // `enabled`, appended a second occurrence, and the two readers
            // then disagreed from one write. At N>1 only the authoritative
            // line is edited -- moving past a duplicate would hand victory to
            // the un-written line. All other bytes verbatim; the rewritten
            // line's inline comment is deleted (today's behavior, F36 -- it
            // described the old value). Section MOVE placement waits on the
            // arc-2 per-key section column (inert here: new keys append at EOF).
            const IniScan st =
                ScanLineSource(LineSource{&FileLineSourceNext, f}, [&](const std::string& s) {
                    std::string k, v;
                    if (!found && ParseIniLine(s, k, v) && _stricmp(k.c_str(), key) == 0) {
                        lines.push_back(newLine);
                        found = true;
                    } else {
                        lines.push_back(s);
                    }
                });
            std::fclose(f);
            if (st == IniScan::Unreadable) {
                // Mid-stream read error: the collected prefix is TRUNCATED.
                // Rebuilding from it is the 2026-07-02 loss shape one layer
                // deeper (fgets NULL == silent EOF before this primitive).
                UE_LOGW("config: WriteIniValue('%s') SKIPPED -- read error mid-file; "
                        "refusing to rebuild the ini from a truncated prefix", key);
                return false;
            }
        }
    }
    if (!found) {
        // Make sure the appended key sits on its own line even if the file's last
        // line had no trailing newline.
        if (!lines.empty() && !lines.back().empty() && lines.back().back() != '\n')
            lines.back() += "\n";
        lines.push_back(newLine);
    }
    const std::wstring tmp = path + L".new";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w") != 0 || !f) {
        UE_LOGW("config: WriteIniValue('%s') could not open multivoid.ini.new for write", key);
        return false;
    }
    // Every write checked BEFORE the swap: a disk-full/IO-error .new must never be
    // atomically installed over the good ini (that would be the one data-loss path
    // this whole function exists to close -- audit 2026-07-02).
    bool wrote = true;
    for (const auto& l : lines)
        if (std::fputs(l.c_str(), f) == EOF) { wrote = false; break; }
    if (std::ferror(f)) wrote = false;
    if (std::fclose(f) != 0) wrote = false;
    if (!wrote) {
        ::DeleteFileW(tmp.c_str());
        UE_LOGW("config: WriteIniValue('%s') writing multivoid.ini.new FAILED (disk?) -- "
                "ini left unchanged", key);
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        UE_LOGW("config: WriteIniValue('%s') atomic swap failed (err=%lu) -- ini left "
                "unchanged, multivoid.ini.new kept", key, ::GetLastError());
        return false;
    }
    UE_LOGI("config: persisted %s=%s", key, safe.c_str());
    return true;
}

bool WriteIniValue(const char* key, const char* value) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    return WriteIniValueAt(IniPath(), key, value);
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
// display mask that prints "DEFAULT" instead of the raw VPS address.
static constexpr const char* kBuiltinMasterUrl    = coop::net::kOfficialMasterUrl;
static constexpr const char* kBuiltinSignalingUrl = coop::net::kOfficialSignalingUrl;

// The custom-master gate. net.master.custom = 1/true/yes/on opts out of the hardcoded
// VPS endpoints and uses the ini's net.master / net.signaling instead. Default OFF ->
// the built-in VPS endpoints win (a stale net.master in the ini is ignored unless the
// gate is set), which is what makes a no-config native install Just Work. An env
// override (VOTVCOOP_MASTER_URL / VOTVCOOP_NET_SIGNALING) always takes precedence over
// both (the dev / LAN-test framework).
static bool UseCustomNetMaster() {
    std::string g = ReadIniValue("net.master.custom", "0");
    for (char& c : g) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');  // case-insensitive
    return g == "1" || g == "true" || g == "yes" || g == "on";
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
    // session, so this default only seeds the master-down fallback.)
    std::string sig = ReadEnv("VOTVCOOP_NET_SIGNALING");
    if (sig.empty())
        sig = UseCustomNetMaster() ? ReadIniValue("net.signaling", kBuiltinSignalingUrl)
                                   : std::string(kBuiltinSignalingUrl);
    c.signalingUrl = sig;
    std::string sigtok = ReadEnv("VOTVCOOP_NET_SIGNALING_TOKEN");
    c.signalingToken = sigtok.empty() ? ReadIniValue("net.signaling_token", "") : sigtok;

    // This peer's own signaling identity. Defaults give a working 2-peer
    // test out of the box (host="votvhost", client="votvclient"); a real
    // lobby with multiple clients MUST issue each client a UNIQUE identity
    // (the signaling server registers one connection per identity string).
    std::string ident = ReadEnv("VOTVCOOP_NET_IDENTITY");
    if (ident.empty()) ident = ReadIniValue("net.identity", "");
    if (ident.empty()) {
        if (c.role == coop::net::Role::Host) {
            ident = "votvhost";
        } else {
            // Unique per-process default so two un-configured clients don't
            // COLLIDE on the signaling server -- it registers one connection
            // per identity string, and a duplicate evicts the incumbent
            // (silently breaking the first client). The master server issues
            // real per-peer identities later; this keeps the default safe for
            // ad-hoc multi-client tests. "votvclient-XXXX" = 15 chars (<= the
            // 31-char SetGenericString cap).
            std::random_device rd;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "votvclient-%04x",
                          static_cast<unsigned>(rd() & 0xFFFFu));
            ident = buf;
        }
    }
    c.localIdentity = ident;

    // The host identity a client dials (must equal the host's localIdentity).
    std::string hostId = ReadEnv("VOTVCOOP_NET_HOST_IDENTITY");
    if (hostId.empty()) hostId = ReadIniValue("net.host_identity", "");
    if (hostId.empty()) hostId = "votvhost";
    c.hostIdentity = hostId;

    // ICE candidate sources. STUN (rung 2) defaults to a public server so a
    // real cross-NAT test works; for a same-machine test ICE also connects
    // via host/LAN candidates regardless. TURN (rung 3) is off by default
    // (the master mints ephemeral REST creds; static ini creds are dev-only).
    std::string stun = ReadEnv("VOTVCOOP_NET_STUN");
    c.stunList = stun.empty() ? ReadIniValue("net.stun", "stun.l.google.com:19302") : stun;
    std::string turn = ReadEnv("VOTVCOOP_NET_TURN");
    c.turnList = turn.empty() ? ReadIniValue("net.turn", "") : turn;
    std::string turnUser = ReadEnv("VOTVCOOP_NET_TURN_USER");
    c.turnUser = turnUser.empty() ? ReadIniValue("net.turn_user", "") : turnUser;
    std::string turnPass = ReadEnv("VOTVCOOP_NET_TURN_PASS");
    c.turnPass = turnPass.empty() ? ReadIniValue("net.turn_pass", "") : turnPass;

    // ICE candidate policy: "" / "all" (default) / "relay" / "disable" /
    // "default". "relay" forces the TURN relay path (privacy, or to validate
    // coturn end-to-end). Mapped to IceEnable in Session::StartP2P.
    std::string ice = ReadEnv("VOTVCOOP_NET_ICE");
    c.iceMode = ice.empty() ? ReadIniValue("net.ice", "") : ice;

    // Console-visible diagnostic: any endpoint on the OFFICIAL VPS host prints
    // as "DEFAULT" -- the connect console must not advertise the raw address
    // (the session_manager DisplayMaster twin; user 2026-06-10). A custom
    // endpoint prints verbatim (its operator debugs with it).
    auto maskOfficial = [](const std::string& v) -> std::string {
        std::string host = coop::net::kOfficialMasterUrl;
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host.resize(colon);
        return v.rfind(host, 0) == 0 ? std::string("DEFAULT") : v;
    };
    UE_LOGI("config: P2P fields -- identity='%s' host='%s' signaling='%s' stun='%s'",
            c.localIdentity.c_str(), c.hostIdentity.c_str(),
            maskOfficial(c.signalingUrl).c_str(), maskOfficial(c.stunList).c_str());
}

coop::net::Config ReadNetConfig(bool& enabled) {
    coop::net::Config c;
    std::string role = ReadEnv("VOTVCOOP_NET_ROLE");
    if (role.empty()) role = ReadIniValue("net.role", "");
    enabled = (role == "host" || role == "client");
    c.role = (role == "client") ? coop::net::Role::Client : coop::net::Role::Host;

    std::string peer = ReadEnv("VOTVCOOP_NET_PEER");
    c.peerIp = peer.empty() ? ReadIniValue("net.peer", "127.0.0.1") : peer;

    std::string port = ReadEnv("VOTVCOOP_NET_PORT");
    if (port.empty()) port = ReadIniValue("net.port", "");
    if (!port.empty()) {
        // strtoul returns unsigned long; a cast to uint16_t silently wraps
        // values >65535 to the wrong port. Range-check before commit.
        const unsigned long raw = std::strtoul(port.c_str(), nullptr, 10);
        if (raw == 0 || raw > 65535) {
            UE_LOGW("config: VOTVCOOP_NET_PORT/net.port='%s' out of [1,65535] -- "
                    "ignoring (keeping default %u)", port.c_str(), c.port);
        } else {
            c.port = static_cast<uint16_t>(raw);
        }
    }

    // --- P2P (zero-open-ports) topology --------------------------------------
    // net.topology = "lan" (default, rung 0/1 IP) or "p2p" (rungs 1-3 ICE).
    std::string topo = ReadEnv("VOTVCOOP_NET_TOPOLOGY");
    if (topo.empty()) topo = ReadIniValue("net.topology", "lan");
    c.topology = (topo == "p2p" || topo == "P2P")
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
    std::string m = ReadEnv("VOTVCOOP_MASTER_URL");
    if (!m.empty()) return m;
    if (UseCustomNetMaster()) {
        std::string v = ReadIniValue("net.master", kBuiltinMasterUrl);
        // "DEFAULT" sentinel (the shipped release ini): resolves to the official
        // server even under the custom gate -- the ini never needs the raw VPS
        // address spelled out.
        std::string lower = v;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (v.empty() || lower == "default") return kBuiltinMasterUrl;
        return v;
    }
    return kBuiltinMasterUrl;
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
    std::string nick = ReadEnv("VOTVCOOP_NET_NICK");
    // T7 (ini rework): the MY-NAME default is the shared registry constant --
    // never a per-site literal (the 4-of-10-defaults-wrong sketch, design F19).
    if (nick.empty()) nick = ReadIniValue("net.nick", coop::config_registry::kMyNameDefault);
    return std::wstring(nick.begin(), nick.end());
}

std::string ReadPlayerSkin() {
    // v93 skins: the persisted body-skin choice, stored NEXT TO the player identity
    // (multivoid.ini "player_skin=", same file as player_guid -- user 2026-07-02).
    // v95: a NEW identity (absent/malformed key) rolls a RANDOM starter from the
    // curated converter-skin list (user: "для НОВЫХ пиров случайный скин из списка"),
    // filtered to paks present on this install -- hl_einstein_v1sc when none is.
    // Persisted immediately (like the guid), so the roll happens ONCE per identity.
    std::string skin = ReadIniValue("player_skin", "");
    if (!coop::skins::IsValidSkinName(skin)) {
        skin = coop::skins::PickRandomStarterSkin();
        // Truthful persist log (F43 fix): the old line said "persisted"
        // unconditionally -- false on a locked file.
        const bool persisted = WriteIniValue("player_skin", skin.c_str());
        UE_LOGI("config: player_skin absent/invalid -> random starter '%s' (%s)", skin.c_str(),
                persisted ? "persisted to multivoid.ini" : "SESSION-ONLY -- ini write failed");
    }
    return skin;
}

std::string ReadPlayerGuid() {
    // Durable per-INSTALL player identity for the host-side per-player inventory
    // (coop_players/<guid>.json). Read from multivoid.ini "player_guid="; generate +
    // persist on first launch / if absent or malformed. 32 lowercase hex chars (128 bits).
    // Per-install identity is the accepted tradeoff (design 2.3 "go with guid"): a reinstall
    // or a different PC = a fresh inventory unless the player_guid= line is copied over.
    std::string guid = ReadIniValue("player_guid", "");
    bool ok = guid.size() == 32;
    if (ok) {
        for (char c : guid)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { ok = false; break; }
    }
    if (!ok) {
        // std::random_device is CSPRNG-backed on MSVC -- ample for a stable identity (no
        // crypto guarantee needed). 4x32 bits -> 32 hex chars. Avoids a bcrypt include.
        std::random_device rd;
        static const char kHex[] = "0123456789abcdef";
        guid.clear();
        guid.reserve(32);
        for (int w = 0; w < 4; ++w) {
            const uint32_t r = rd();
            for (int n = 28; n >= 0; n -= 4) guid.push_back(kHex[(r >> n) & 0xF]);
        }
        const bool persisted = WriteIniValue("player_guid", guid.c_str());
        UE_LOGI("config: generated new player_guid=%s (%s)", guid.c_str(),
                persisted ? "persisted to multivoid.ini" : "SESSION-ONLY -- ini write failed");
    }
    return guid;
}

// ---- boolean ini flags (merged from coop/session/ini_config, 2026-07-10) ----
// Case/space/inline-comment tolerant `key=1`/`key=true` matching -- distinct from
// ReadIniValue's edge-trimmed exact parse because flag lines carry inline `; comments`
// pervasively (`garbage_pickup_probe=1   ; v81 morph...`) and the flags predate the
// generic reader. First match wins.

namespace {

// Strip an inline `; comment`, then ALL whitespace, then lowercase. A `;` never
// appears in a real flag value, so cutting at the first `;` is safe.
std::string NormalizeFlagLine(const char* line) {
    std::string s(line);
    if (const size_t c = s.find(';'); c != std::string::npos) s.erase(c);
    s.erase(std::remove_if(s.begin(), s.end(),
                           [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
            s.end());
    for (auto& c : s) c = static_cast<char>(::tolower(c));
    return s;
}

// Scan an ini for `key=...`:  +1 = true,  0 = absent,  -1 = false. Match
// semantics UNCHANGED in arc 1 (first RECOGNIZED value, ci, comment-stripped --
// the F25 legacy rule; it dies with the arc-2 layer flip). Lines are unbounded
// via the primitive; no lock here -- public wrappers hold g_iniMutex.
int LookupTriStateAt(const std::wstring& path, const char* key) {
    const std::string trueForm  = std::string(key) + "=1";
    const std::string trueAlt   = std::string(key) + "=true";
    const std::string falseForm = std::string(key) + "=0";
    const std::string falseAlt  = std::string(key) + "=false";
    int verdict = 0;
    ScanIniFileAt(path, [&](const std::string& line) {
        if (verdict != 0) return;
        const std::string s = NormalizeFlagLine(line.c_str());
        if (s == trueForm || s == trueAlt) { verdict = 1; return; }
        if (s == falseForm || s == falseAlt) { verdict = -1; }
    });
    return verdict;
}

int LookupTriState(const char* key) {
    std::lock_guard<std::mutex> lk(g_iniMutex);
    return LookupTriStateAt(IniPath(), key);
}

}  // namespace

bool MasterEnabled() {
    // ABSENT defaults to enabled (= granular switches decide). Only an explicit
    // `enabled=0` forces all dev features off.
    return LookupTriState("enabled") != -1;
}

bool IsIniKeyTrue(const char* key) {
    return LookupTriState(key) == 1;
}

// ---- dev selftest seams (config corpus instrument; probes are RULE-2-exempt) ----
// Path-parameterized twins of the two readers + the raw line list + a failing-
// source scan, so the env-gated autotest (autotest_config.cpp) can run the REAL
// lexer over a corpus of ini files and prove the tri-state branches. Not for
// product use: product code reads only the module-dir ini via the public API.

IniSelftestRead SelftestReadValue(const std::wstring& path, const char* key) {
    IniScan st = IniScan::Ok;
    IniSelftestRead r;
    const std::string sentinel = "\x01<absent>";
    r.value = ReadIniValueAt(path, key, sentinel.c_str(), &st);
    r.found = (r.value != sentinel);
    if (!r.found) r.value.clear();
    r.scan = static_cast<int>(st);
    return r;
}

int SelftestFlagTriState(const std::wstring& path, const char* key) {
    return LookupTriStateAt(path, key);
}

bool SelftestWriteValue(const std::wstring& path, const char* key, const char* value) {
    return WriteIniValueAt(path, key, value);
}

int SelftestListLines(const std::wstring& path, std::vector<std::string>& out) {
    const IniScan st = ScanIniFileAt(path, [&](const std::string& line) { out.push_back(line); });
    return static_cast<int>(st);
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

int SelftestScanWithFailure(int failAfterLines) {
    FailingSourceCtx ctx{failAfterLines};
    const IniScan st =
        ScanLineSource(LineSource{&FailingSourceNext, &ctx}, [](const std::string&) {});
    // The branch under test: a mid-stream error must yield Unreadable (2),
    // never a clean Ok that would read as ABSENT downstream (design F38).
    return static_cast<int>(st);
}

}  // namespace coop::config
