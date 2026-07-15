// coop/console_state_sync.cpp -- see coop/console_state_sync.h.

#include "coop/interactables/console_state_sync.h"

#include "coop/interactables/device_occupancy.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_catch_sync.h"
#include "coop/dev/desk_diag.h"  // [dev] JOIN-ADOPT pre-apply baseline hook (no-op unless desk_diag=1)

#include "ue_wrap/console_desk.h"
#include "ue_wrap/log.h"
#include "ue_wrap/space_renderer.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace coop::console_state_sync {
namespace {

namespace SR = ue_wrap::space_renderer;
namespace CD = ue_wrap::console_desk;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kSkyPoll       = std::chrono::milliseconds(1000);  // host set poll + client sweep
constexpr auto kDeskPoll      = std::chrono::milliseconds(1000);  // claimed cadence + unclaimed edge poll
constexpr auto kDishInterval  = std::chrono::milliseconds(330);   // ~3 Hz claimed stream
constexpr int  kMaxSkyRows    = 15;                               // 5 parts -- far above the ~12 native cap
const wchar_t* const kDeskClaim = L"desk";

// ---- sky-signal state -------------------------------------------------------
// g_skyMirror: HOST = the last broadcast set; CLIENT = the last wire-applied
// set (the authoritative mirror the sweep + catch detector compare against).
std::vector<SR::SignalRow> g_skyMirror;
bool g_haveWireSet = false;          // client: first snapshot landed
uint8_t g_skyGen = 0;                // host: snapshot generation
void* g_suppressedOn = nullptr;      // client: the instance whose roller we killed
Clock::time_point g_nextSkyPoll{};

// Client part-assembly (snapshots arrive as <=5 parts of <=3 rows). `started`
// bounds a stale half-assembly's lifetime (audit N-1: an abandoned assembly
// whose uint8 gen happens to wrap-collide with a much later snapshot would
// otherwise contribute leftover rows) -- anything older than 5 s is reset
// before the new part is taken.
struct PartAssembly {
    uint8_t gen = 0, parts = 0, got = 0;
    bool present[5] = {};
    std::vector<SR::SignalRow> rows;
    Clock::time_point started{};
};
PartAssembly g_assembly;
constexpr auto kAssemblyTTL = std::chrono::seconds(5);

// ---- desk state -------------------------------------------------------------
CD::Scalars g_deskLastSent;          // owner-side dedupe
CD::Scalars g_deskEdgeLast;          // unclaimed discrete-edge detector
bool g_deskEdgePrimed = false;
Clock::time_point g_nextDeskPoll{};

// ---- desk log lines (v70) ---------------------------------------------------
// The PRODUCER diff baseline: the full coord_coordLog2Text (BP-capped at 1000
// chars) at the last poll / apply. First sight primes without replaying
// history (a session-old terminal must not flood the wire at install).
std::wstring g_logBaseline;
bool g_logPrimed = false;
// The BP cap is 1000 chars; read a hair over so the window never truncates.
constexpr size_t kLogReadMax = 1100;

// ---- dish aim ---------------------------------------------------------------
CD::DishAim g_dishLastSent;
Clock::time_point g_nextDish{};

bool RowIdentityEq(const SR::SignalRow& a, const SR::SignalRow& b) {
    // Exact float equality is CORRECT here: every non-host copy of a row was
    // byte-copied off the wire from the host's roll, so identities compare
    // bit-identical (the v52 eid lesson: never fuzzy-match identities).
    return a.x == b.x && a.y == b.y && a.z == b.z && a.frequency == b.frequency;
}

bool RowContentEq(const SR::SignalRow& a, const SR::SignalRow& b) {
    // Alpha/lifetimes EXCLUDED: they tick every frame; comparing them would
    // turn the 1 Hz host poll into a 1 Hz unconditional broadcast.
    return RowIdentityEq(a, b) && a.type == b.type && a.strength == b.strength &&
           a.frequencySpread == b.frequencySpread && a.polarity == b.polarity &&
           a.polaritySpread == b.polaritySpread && a.direction == b.direction &&
           a.objectName == b.objectName;
}

bool SetContentEq(const std::vector<SR::SignalRow>& a, const std::vector<SR::SignalRow>& b) {
    if (a.size() != b.size()) return false;
    // Order-insensitive: deleteSignal compacts the arrays, so index order can
    // differ between polls without any real change. O(n^2) over <=~12 rows.
    for (const auto& ra : a) {
        bool found = false;
        for (const auto& rb : b)
            if (RowContentEq(ra, rb)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

void RowToWire(const SR::SignalRow& r, coop::net::WireSkySignal& w) {
    std::memset(&w, 0, sizeof(w));
    w.x = r.x; w.y = r.y; w.z = r.z;
    w.type = r.type;
    w.strength = r.strength;
    w.frequency = r.frequency;
    w.frequencySpread = r.frequencySpread;
    w.polarity = r.polarity;
    w.polaritySpread = r.polaritySpread;
    w.alpha = r.alpha;
    w.lifeTime = r.lifeTime;
    w.maxLifetime = r.maxLifetime;
    w.direction = r.direction ? 1 : 0;
    size_t n = 0;
    for (; n < r.objectName.size() && n < sizeof(w.objectName); ++n)
        w.objectName[n] = static_cast<char>(r.objectName[n]);  // rolled names are ASCII
    w.nameLen = static_cast<uint8_t>(n);
    if (r.objectName.size() > sizeof(w.objectName)) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            UE_LOGW("console_state: sky-signal objectName '%ls' exceeds the 15-char wire "
                    "budget -- truncated (receiver FName will differ)", r.objectName.c_str());
        }
    }
}

SR::SignalRow WireToRow(const coop::net::WireSkySignal& w) {
    SR::SignalRow r;
    r.x = w.x; r.y = w.y; r.z = w.z;
    r.type = w.type;
    r.strength = w.strength;
    r.frequency = w.frequency;
    r.frequencySpread = w.frequencySpread;
    r.polarity = w.polarity;
    r.polaritySpread = w.polaritySpread;
    r.alpha = w.alpha;
    r.lifeTime = w.lifeTime;
    r.maxLifetime = w.maxLifetime;
    r.direction = w.direction != 0;
    const size_t n = w.nameLen <= sizeof(w.objectName) ? w.nameLen : sizeof(w.objectName);
    r.objectName.assign(w.objectName, w.objectName + n);
    return r;
}

bool WireRowFinite(const coop::net::WireSkySignal& w) {
    const float vals[] = { w.x, w.y, w.z, w.strength, w.frequency, w.frequencySpread,
                           w.polarity, w.polaritySpread, w.alpha, w.lifeTime,
                           w.maxLifetime };
    for (float v : vals)
        if (!std::isfinite(v)) return false;
    return true;
}

// Send the full current set as parts. `toSlot` < 0 broadcasts; else
// point-to-point (the connect snapshot).
void SendSkySnapshot(coop::net::Session* s, const std::vector<SR::SignalRow>& set,
                     uint8_t gen, int toSlot) {
    const size_t total = set.size() <= kMaxSkyRows ? set.size() : kMaxSkyRows;
    if (set.size() > kMaxSkyRows)
        UE_LOGW("console_state: sky set %zu rows > cap %d -- excess dropped from the wire",
                set.size(), kMaxSkyRows);
    const uint8_t parts = static_cast<uint8_t>(total == 0 ? 1 : (total + 2) / 3);
    size_t idx = 0;
    for (uint8_t part = 0; part < parts; ++part) {
        coop::net::SkySignalStatePayload p{};
        p.gen = gen;
        p.part = part;
        p.parts = parts;
        p.totalCount = static_cast<uint8_t>(total);
        for (int i = 0; i < 3 && idx < total; ++i, ++idx) {
            RowToWire(set[idx], p.rows[i]);
            ++p.count;
        }
        if (toSlot < 0)
            s->SendReliable(coop::net::ReliableKind::SkySignalState, &p, sizeof(p));
        else
            s->SendReliableToSlot(toSlot, coop::net::ReliableKind::SkySignalState, &p, sizeof(p));
    }
}

bool ScalarsDiffer(const CD::Scalars& a, const CD::Scalars& b) {
    return std::memcmp(&a, &b, sizeof(CD::Scalars)) != 0;
}

// The PLAYER-INPUT subset for the unclaimed edge detector: discrete buttons/
// toggles ONLY. The four filter knob floats were originally here under the
// phase-2 claim "only player input writes them" -- FALSIFIED by the smoke of
// 2026-06-12 19:07: both peers, standing idle with the desk unclaimed,
// broadcast (dlPoFilterOffset,dlFrFilterOffset) edges once per second -- the
// GAME drifts the knob floats continuously. That made every peer a 1 Hz
// DeskState sender forever; the receivers' ApplyCoordLogTail append-on-no-
// overlap then grew the BP coordLog unboundedly on every peer = the round-3
// RAM balloon + the duplicated/gappy mirror log. Knob floats are claim-owner
// state: they ride the 1 Hz OWNER stream (ScalarsDiffer) like the rest of
// the analog state; an unclaimed knob write stays local by design (v64
// authority model). The genuinely tick-derived fields (detection needle /
// progress / cooldown / the downloading float's magnitude) stay excluded for
// the same reason they always were.
bool DiscreteDiffer(const CD::Scalars& a, const CD::Scalars& b) {
    // canDL is also OUT (STOLAS RE 2026-06-12: it is DERIVED -- canSaveSignal
    // recomputes it per tick from signal state @313); it would oscillate the
    // moment signal conditions fluctuate, exactly like the knob floats did.
    return a.playVolume != b.playVolume || a.dlPolarityDir != b.dlPolarityDir ||
           a.compMaxLevel != b.compMaxLevel || a.playSelectIndex != b.playSelectIndex ||
           a.dlActiveFrFilter != b.dlActiveFrFilter || a.dlActivePoFilter != b.dlActivePoFilter ||
           a.activePlay != b.activePlay || a.activeDownload != b.activeDownload ||
           a.activeCoords != b.activeCoords || a.activeComp != b.activeComp ||
           (a.dlDownloading > 0.f) != (b.dlDownloading > 0.f);
}

// Name the fields DiscreteDiffer saw flip. A real button press names ONE field
// once; a tick-derived field misclassified into the "player input" set names
// itself at the poll rate forever -- which is how the 2026-06-12 1 Hz edge
// storm (and its ApplyCoordLogTail append RAM balloon) gets pinned to a field
// from a single log line instead of another blind round.
void DescribeDiscreteDiff(const CD::Scalars& a, const CD::Scalars& b,
                          char* out, size_t cap) {
    size_t n = 0;
    out[0] = '\0';
    auto add = [&](const char* name) {
        const size_t l = ::strlen(name);
        if (n + l + 2 >= cap) return;
        if (n) out[n++] = ',';
        ::memcpy(out + n, name, l);
        n += l;
        out[n] = '\0';
    };
    if (a.playVolume != b.playVolume)               add("playVolume");
    if (a.dlPolarityDir != b.dlPolarityDir)         add("dlPolarityDir");
    if (a.compMaxLevel != b.compMaxLevel)           add("compMaxLevel");
    if (a.playSelectIndex != b.playSelectIndex)     add("playSelectIndex");
    if (a.dlActiveFrFilter != b.dlActiveFrFilter)   add("dlActiveFrFilter");
    if (a.dlActivePoFilter != b.dlActivePoFilter)   add("dlActivePoFilter");
    if (a.activePlay != b.activePlay)               add("activePlay");
    if (a.activeDownload != b.activeDownload)       add("activeDownload");
    if (a.activeCoords != b.activeCoords)           add("activeCoords");
    if (a.activeComp != b.activeComp)               add("activeComp");
    if ((a.dlDownloading > 0.f) != (b.dlDownloading > 0.f)) add("dlDownloading>0");
    if (n == 0) add("none");
}

void ScalarsToPayload(const CD::Scalars& sc, uint8_t adopt,
                      coop::net::DeskStatePayload& p) {
    std::memset(&p, 0, sizeof(p));
    p.dlPoFilterOffset = sc.dlPoFilterOffset;
    p.dlFrFilterOffset = sc.dlFrFilterOffset;
    p.dlPoFilterSpeed = sc.dlPoFilterSpeed;
    p.dlFrFilterSpeed = sc.dlFrFilterSpeed;
    p.dlDownloading = sc.dlDownloading;
    p.dlResDetecPercent = sc.dlResDetecPercent;
    p.coordCooldown = sc.coordCooldown;
    p.playVolume = sc.playVolume;
    p.dlPolarityDir = sc.dlPolarityDir;
    p.compMaxLevel = sc.compMaxLevel;
    p.playSelectIndex = sc.playSelectIndex;
    p.dlActiveFrFilter = sc.dlActiveFrFilter ? 1 : 0;
    p.dlActivePoFilter = sc.dlActivePoFilter ? 1 : 0;
    p.activePlay = sc.activePlay ? 1 : 0;
    p.activeDownload = sc.activeDownload ? 1 : 0;
    p.activeCoords = sc.activeCoords ? 1 : 0;
    p.activeComp = sc.activeComp ? 1 : 0;
    p.coordIsPing = sc.coordIsPing ? 1 : 0;
    p.adopt = adopt;
    // dlDecoded/dlPolarity: ADOPT-ONLY consumers (the joiner catch-up); filled
    // on every send for simplicity, ignored by live appliers.
    float decoded = 0; int32_t polarity = -1;
    if (CD::ReadDownloadProgress(decoded, polarity)) {
        p.dlDecoded = decoded;
        p.dlPolarity = polarity;
    } else {
        p.dlPolarity = -1;
    }
}

CD::Scalars PayloadToScalars(const coop::net::DeskStatePayload& p) {
    CD::Scalars sc;
    sc.dlPoFilterOffset = p.dlPoFilterOffset;
    sc.dlFrFilterOffset = p.dlFrFilterOffset;
    sc.dlPoFilterSpeed = p.dlPoFilterSpeed;
    sc.dlFrFilterSpeed = p.dlFrFilterSpeed;
    sc.dlDownloading = p.dlDownloading;
    sc.dlResDetecPercent = p.dlResDetecPercent;
    sc.coordCooldown = p.coordCooldown;
    sc.playVolume = p.playVolume;
    sc.dlPolarityDir = p.dlPolarityDir;
    sc.compMaxLevel = p.compMaxLevel;
    sc.playSelectIndex = p.playSelectIndex;
    sc.dlActiveFrFilter = p.dlActiveFrFilter != 0;
    sc.dlActivePoFilter = p.dlActivePoFilter != 0;
    sc.activePlay = p.activePlay != 0;
    sc.activeDownload = p.activeDownload != 0;
    sc.activeCoords = p.activeCoords != 0;
    sc.activeComp = p.activeComp != 0;
    sc.coordIsPing = p.coordIsPing != 0;
    return sc;
}

void SendDeskState(coop::net::Session* s, const CD::Scalars& sc, uint8_t adopt, int toSlot) {
    coop::net::DeskStatePayload p{};
    ScalarsToPayload(sc, adopt, p);
    if (toSlot < 0)
        s->SendReliable(coop::net::ReliableKind::DeskState, &p, sizeof(p));
    else
        s->SendReliableToSlot(toSlot, coop::net::ReliableKind::DeskState, &p, sizeof(p));
}

// ---- desk log lines (v70): the producer diff --------------------------------
// The terminal text is ONE local monotone sequence (writeToCoordLog_2 appends
// + trims to the last 1000 chars), so the largest suffix(baseline) ==
// prefix(cur) overlap recovers the appended text EXACTLY on the producer.
// (A self-similar repeated line can over-match the overlap and eat its own
// duplicate -- only the animated bar lines repeat verbatim, and those are
// filtered below anyway.) Receiver-side reconstruction died with the v64 tail
// window (RULE 2): only complete event lines ride the wire now.

// The animated line families regenerate per peer from mirrored scalars
// (cooldown/isPing via DeskState; the CR cursor line only while dragging) --
// shipping them would double-write every mirror's terminal at 5 Hz.
bool IsAnimatedLogLine(const std::wstring& line) {
    static const wchar_t* const kPrefixes[] = {
        L"CDOWN: [", L"AREA SCAN: [", L"APPROXIMATION:", L"ANALYSIS:", L"CR:[",
    };
    for (const wchar_t* p : kPrefixes)
        if (line.compare(0, ::wcslen(p), p) == 0) return true;
    return false;
}

void SendLogLine(coop::net::Session* s, const std::wstring& line) {
    coop::net::DeskLogLinePayload p{};
    size_t n = 0;
    for (; n < line.size() && n < sizeof(p.line); ++n)
        p.line[n] = static_cast<char>(line[n]);  // the BP lines are ASCII
    if (line.size() > sizeof(p.line))
        UE_LOGW("console_state: desk log line truncated to %zu chars on the wire",
                sizeof(p.line));
    p.len = static_cast<uint8_t>(n);
    if (n) s->SendReliable(coop::net::ReliableKind::DeskLogLine, &p, sizeof(p));
}

// Poll the terminal, ship NEW complete event lines (1 Hz from the desk poll;
// also flushed before a wire apply so the apply's baseline advance can't
// swallow a pending local line).
void ProduceLogLines(coop::net::Session* s) {
    // Steady-state (log unchanged -- almost every poll): one in-place compare
    // against the live engine FString, zero allocations (perf audit
    // 2026-06-12 IMPROVE-1; the wstring build below runs only on change).
    if (g_logPrimed && CD::CoordLogTailEquals(g_logBaseline, kLogReadMax)) return;
    const std::wstring cur = CD::ReadCoordLogTail(kLogReadMax);
    if (!g_logPrimed) {
        // First sight: prime without replaying history (the terminal is
        // session-local even in SP -- a joiner starts empty by design).
        g_logBaseline = cur;
        g_logPrimed = true;
        return;
    }
    if (cur == g_logBaseline) return;
    // Largest suffix of the baseline that prefixes cur == the shared region.
    size_t k = g_logBaseline.size() < cur.size() ? g_logBaseline.size() : cur.size();
    for (; k > 0; --k) {
        if (cur.compare(0, k, g_logBaseline, g_logBaseline.size() - k, k) == 0) break;
    }
    std::wstring fresh = cur.substr(k);
    g_logBaseline = cur;
    // Split on CRLF; writeToCoordLog_2 appends line+CRLF atomically on the
    // game thread, so the fresh region is always whole lines.
    size_t pos = 0;
    while (pos < fresh.size()) {
        size_t eol = fresh.find_first_of(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = fresh.size();
        if (eol > pos) {
            const std::wstring line = fresh.substr(pos, eol - pos);
            if (!IsAnimatedLogLine(line)) {
                SendLogLine(s, line);
                UE_LOGI("console_state: desk log event line shipped (%zu chars)", line.size());
            }
        }
        pos = (eol == fresh.size()) ? eol : fresh.find_first_not_of(L"\r\n", eol);
        if (pos == std::wstring::npos) break;
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    const bool srUp = SR::EnsureResolved() && SR::Instance();
    const bool cdUp = CD::EnsureResolved() && CD::Instance();
    if (!srUp && !cdUp) return;
    const bool isHost = (s->role() == coop::net::Role::Host);
    const auto now = Clock::now();

    // ---- SKY SIGNALS ----
    if (srUp && now >= g_nextSkyPoll) {
        g_nextSkyPoll = now + kSkyPoll;
        if (isHost) {
            // The authority: broadcast the set on ANY content change.
            std::vector<SR::SignalRow> cur;
            if (SR::ReadSignals(cur) && s->connected() && !SetContentEq(cur, g_skyMirror)) {
                ++g_skyGen;
                SendSkySnapshot(s, cur, g_skyGen, -1);
                g_skyMirror = std::move(cur);
                UE_LOGI("console_state: sky snapshot gen=%u broadcast (%zu row(s))",
                        static_cast<unsigned>(g_skyGen), g_skyMirror.size());
            }
        } else {
            // CLIENT: enforce the roller suppression per instance (a level
            // reload spawns a fresh spaceRenderer with a fresh armed timer).
            void* inst = SR::Instance();
            if (inst && inst != g_suppressedOn && SR::KillClientSpawnTimer()) {
                g_suppressedOn = inst;
                UE_LOGI("console_state: client sky roller timer killed (instance %p)", inst);
            }
            if (g_haveWireSet) {
                // (The CATCH detector/relay moved to coop/signal_catch_sync in
                // v70 -- the v64 vanish-only detector here relayed natural
                // EXPIRY as a catch, lethal once the replay slews dishes.)
                std::vector<SR::SignalRow> cur;
                if (SR::ReadSignals(cur)) {
                    // DIVERGENCE SWEEP: a local row the wire never expressed
                    // (a pre-kill self-roll / a kill miss) gets deleted -- the
                    // client set stays a pure mirror.
                    for (const auto& c : cur) {
                        bool known = false;
                        for (const auto& m : g_skyMirror)
                            if (RowIdentityEq(c, m)) { known = true; break; }
                        if (!known) {
                            SR::RemoveSignalByIdentity(c.x, c.y, c.z, c.frequency);
                            UE_LOGI("console_state: swept non-wire sky row (%.0f, %.0f, %.0f)",
                                    c.x, c.y, c.z);
                        }
                    }
                }
            }
        }
    }

    if (!s->connected()) return;  // everything below is wire traffic

    // ---- DESK + DISH (claim-owner streams / unclaimed edges) ----
    if (cdUp && now >= g_nextDeskPoll) {
        g_nextDeskPoll = now + kDeskPoll;
        CD::Scalars cur;
        if (CD::ReadScalars(cur)) {
            const bool owner = coop::device_occupancy::LocalHolds(kDeskClaim);
            if (owner) {
                // Claimed cadence: anything changed -> stream (1 Hz).
                if (ScalarsDiffer(cur, g_deskLastSent)) {
                    SendDeskState(s, cur, 0, -1);
                    g_deskLastSent = cur;
                }
            } else if (g_deskEdgePrimed && DiscreteDiffer(cur, g_deskEdgeLast)) {
                // Unclaimed: a LOCAL discrete flip (physical button press by
                // this peer; wire applies prime g_deskEdgeLast so an applied
                // edge never re-broadcasts).
                SendDeskState(s, cur, 0, -1);
                g_deskLastSent = cur;
                char diff[192];
                DescribeDiscreteDiff(cur, g_deskEdgeLast, diff, sizeof(diff));
                UE_LOGI("console_state: desk button edge broadcast (%s)", diff);
            }
            g_deskEdgeLast = cur;
            g_deskEdgePrimed = true;
        }
        // Desk log lines (v70): EVERY peer produces -- event lines originate
        // where the action ran (the holder's catch/errors, any peer's delete
        // button); the animated-prefix filter keeps mirror-regenerated lines
        // off the wire.
        ProduceLogLines(s);
    }
    if (cdUp && coop::device_occupancy::LocalHolds(kDeskClaim) && now >= g_nextDish) {
        g_nextDish = now + kDishInterval;
        // Field-compare dedupe (NOT memcmp: DishAim has tail padding after the
        // v70 direction byte; indeterminate stack padding would defeat the
        // dedupe and turn the 3 Hz poll into an unconditional stream).
        // v109: the LIVE cursor (viewCoordinate) is NO LONGER on this reliable lane --
        // it streams on the unreliable DeskCursorPose lane (coop::desk_cursor_sync),
        // interpolated on the mirror. This lane now carries ONLY the discrete COMMITTED
        // locks, so aimEq compares committed-only: a cursor SWEEP no longer triggers a
        // reliable re-send (only a commit does). Two authors on viewCoordinate was the
        // dupe shape -- the old p.viewX/p.viewY writes are DELETED here (RULE 2).
        auto aimEq = [](const CD::DishAim& a, const CD::DishAim& b) {
            return a.c0X == b.c0X && a.c0Y == b.c0Y &&
                   a.c1X == b.c1X && a.c1Y == b.c1Y &&
                   a.c2X == b.c2X && a.c2Y == b.c2Y &&
                   a.selected == b.selected && a.direction == b.direction;
        };
        CD::DishAim aim;
        if (CD::ReadDishAim(aim) && !aimEq(aim, g_dishLastSent)) {
            coop::net::DishAimStatePayload p{};
            p.c0X = aim.c0X; p.c0Y = aim.c0Y;
            p.c1X = aim.c1X; p.c1Y = aim.c1Y;
            p.c2X = aim.c2X; p.c2Y = aim.c2Y;
            p.selected = aim.selected;
            p.direction = aim.direction;
            s->SendReliable(coop::net::ReliableKind::DishAimState, &p, sizeof(p));
            g_dishLastSent = aim;
        }
    }
}

void OnSkySignalState(const coop::net::SkySignalStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() == coop::net::Role::Host) return;  // host-auth: host never applies
    if (senderSlot != 0) {
        UE_LOGW("console_state: SkySignalState from non-host slot %u -- dropping",
                static_cast<unsigned>(senderSlot));
        return;
    }
    if (p.parts == 0 || p.parts > 5 || p.part >= p.parts || p.count > 3) return;
    for (uint8_t i = 0; i < p.count; ++i)
        if (!WireRowFinite(p.rows[i])) {
            UE_LOGW("console_state: non-finite sky row -- dropping snapshot part");
            return;
        }
    const auto asmNow = Clock::now();
    if (g_assembly.gen != p.gen || g_assembly.parts != p.parts ||
        (g_assembly.got > 0 && asmNow - g_assembly.started > kAssemblyTTL)) {
        g_assembly = {};
        g_assembly.gen = p.gen;
        g_assembly.parts = p.parts;
        g_assembly.started = asmNow;
    }
    if (g_assembly.present[p.part]) return;  // duplicate part
    g_assembly.present[p.part] = true;
    ++g_assembly.got;
    for (uint8_t i = 0; i < p.count; ++i)
        g_assembly.rows.push_back(WireToRow(p.rows[i]));
    if (g_assembly.got < g_assembly.parts) return;

    // Snapshot complete -- reconcile the local set to it.
    if (!SR::EnsureResolved() || !SR::Instance()) {
        // World not up yet (shouldn't happen post-world-ready; defensive).
        g_assembly = {};
        return;
    }
    // CATCH-vs-SNAPSHOT race (v70): an in-flight local catch must outrank a
    // stale snapshot row -- signal_catch_sync runs its detector now and strips
    // recently-caught identities from the incoming set.
    coop::signal_catch_sync::NoteIncomingSnapshot(g_assembly.rows);
    SR::ApplyStats stats;
    if (SR::ApplySignalSet(g_assembly.rows, stats)) {
        g_skyMirror = std::move(g_assembly.rows);
        g_haveWireSet = true;
        UE_LOGI("console_state: sky snapshot gen=%u applied -- +%d -%d =%d (total %zu)",
                static_cast<unsigned>(g_assembly.gen), stats.added, stats.removed,
                stats.kept, g_skyMirror.size());
    }
    g_assembly = {};
}

void OnDeskState(const coop::net::DeskStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // Finite-validate before any engine write (the WireRowFinite/PayloadFinite
    // discipline -- a NaN filter offset/cooldown must never land in BP floats).
    const float vals[] = { p.dlPoFilterOffset, p.dlFrFilterOffset, p.dlPoFilterSpeed,
                           p.dlFrFilterSpeed, p.dlDownloading, p.dlResDetecPercent,
                           p.coordCooldown, p.dlDecoded };
    for (float v : vals)
        if (!std::isfinite(v)) return;
    if (p.adopt && senderSlot != 0) return;  // adopt snapshots are host-only
    // While the desk is CLAIMED, the holder is the sole authority -- drop a
    // non-holder's live edge (stale/buggy double-stream).
    const uint8_t holder = coop::device_occupancy::HolderOf(kDeskClaim);
    if (!p.adopt && holder != 0xFF && senderSlot != holder) return;
    // The local holder ignores incoming live state (it IS the authority).
    if (!p.adopt && coop::device_occupancy::LocalHolds(kDeskClaim)) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;

    // [dev] desk_diag JOIN-ADOPT: capture the client's PRE-ADOPT local scalars
    // BEFORE WriteScalars overwrites them with the host seed (join-seed vs drift).
    if (p.adopt && s->role() == coop::net::Role::Client)
        coop::dev::desk_diag::NoteJoinAdopt();

    CD::Scalars sc = PayloadToScalars(p);
    if (CD::WriteScalars(sc)) {
        // Prime BOTH detectors so this apply never reads as a local edge and
        // never echo-streams back (house lastKnown pattern).
        g_deskEdgeLast = sc;
        g_deskEdgePrimed = true;
        g_deskLastSent = sc;
        // dlDecoded/dlPolarity are ADOPT-ONLY (live decoded self-accrues on
        // every armed peer -- see the protocol v70 note): the joiner queues
        // the host's progress and applies it once its own machine arms.
        if (p.adopt && s->role() == coop::net::Role::Client) {
            coop::signal_catch_sync::SetPendingDownloadAdopt(
                p.dlDecoded, p.dlPolarity, p.dlResDetecPercent);
        }
    }
}

void OnDishAim(const coop::net::DishAimStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // v109: committed-coords only (viewCoordinate lives on the DeskCursorPose lane).
    const float vals[] = { p.c0X, p.c0Y, p.c1X, p.c1Y, p.c2X, p.c2Y };
    for (float v : vals)
        if (!std::isfinite(v)) return;
    // Only the desk-claim holder streams aim; drop anything else.
    const uint8_t holder = coop::device_occupancy::HolderOf(kDeskClaim);
    if (holder == 0xFF || senderSlot != holder) return;
    if (coop::device_occupancy::LocalHolds(kDeskClaim)) return;  // we are the streamer
    if (!CD::EnsureResolved()) return;
    CD::DishAim aim;  // viewX/viewY stay 0 -- WriteDishCommitted ignores them (cursor lane owns viewCoordinate)
    aim.c0X = p.c0X; aim.c0Y = p.c0Y;
    aim.c1X = p.c1X; aim.c1Y = p.c1Y;
    aim.c2X = p.c2X; aim.c2Y = p.c2Y;
    aim.selected = p.selected;
    aim.direction = p.direction;
    CD::WriteDishCommitted(aim);
}

void OnDeskLogLine(const coop::net::DeskLogLinePayload& p, uint8_t senderSlot) {
    (void)senderSlot;  // producer-symmetric; the host relay already fanned out
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const size_t n = p.len <= sizeof(p.line) ? p.len : sizeof(p.line);
    if (n == 0) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    // Flush OUR pending local lines first -- the apply below advances the
    // producer baseline past the whole terminal, which would otherwise
    // swallow an unshipped local event line written in the last poll window.
    ProduceLogLines(s);
    std::wstring line(p.line, p.line + n);
    CD::AppendCoordLog(line);  // writeToCoordLog_2: native CRLF + repaint + cap
    // Echo-proof: advance our own diff baseline past the applied text (the
    // email watermark prime precedent).
    g_logBaseline = CD::ReadCoordLogTail(kLogReadMax);
    g_logPrimed = true;
}

void PrimeDeskEdgeDetector() {
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    CD::Scalars cur;
    if (CD::ReadScalars(cur)) {
        g_deskEdgeLast = cur;
        g_deskEdgePrimed = true;
        g_deskLastSent = cur;
    }
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // Sky set: the joiner starts from the authoritative snapshot (its own
    // save-loaded world rolled nothing -- signals are runtime-only).
    if (SR::EnsureResolved() && SR::Instance()) {
        std::vector<SR::SignalRow> cur;
        if (SR::ReadSignals(cur)) {
            ++g_skyGen;
            SendSkySnapshot(s, cur, g_skyGen, peerSlot);
            g_skyMirror = std::move(cur);
            UE_LOGI("console_state: connect sky snapshot -> slot %d (%zu row(s))",
                    peerSlot, g_skyMirror.size());
        }
    }
    // Desk scalars: adopt snapshot (the persisted blob already rode the v56
    // save transfer; this trues up the live scalars since that save). The
    // joiner's terminal history is NOT replayed -- the coordLog is session-
    // local even in SP (analogPanelsData carries no log field).
    if (CD::EnsureResolved() && CD::Instance()) {
        CD::Scalars cur;
        if (CD::ReadScalars(cur)) {
            SendDeskState(s, cur, 1, peerSlot);
        }
        // v109: the committed triangulation locks (DishAimState) are change-gated, so
        // an IDLE occupant sends nothing and a joiner would see STALE locks. Snapshot
        // the current committed coords on connect (the live cursor needs no snapshot --
        // the DeskCursorPose stream re-primes it within one send interval).
        CD::DishAim aim;
        if (CD::ReadDishAim(aim)) {
            coop::net::DishAimStatePayload p{};
            p.c0X = aim.c0X; p.c0Y = aim.c0Y;
            p.c1X = aim.c1X; p.c1Y = aim.c1Y;
            p.c2X = aim.c2X; p.c2Y = aim.c2Y;
            p.selected = aim.selected;
            p.direction = aim.direction;
            s->SendReliableToSlot(peerSlot, coop::net::ReliableKind::DishAimState, &p, sizeof(p));
        }
    }
}

void OnDisconnect() {
    // CLIENT: restore the native sky roller (one reflected spawnSignal()
    // re-arms the BP's own loop) -- the world returns to SP behavior.
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Client && g_suppressedOn) {
        if (SR::RestoreRoller())
            UE_LOGI("console_state: client sky roller restored (native loop re-armed)");
    }
    g_suppressedOn = nullptr;
    g_skyMirror.clear();
    g_haveWireSet = false;
    g_assembly = {};
    g_deskLastSent = {};
    g_deskEdgeLast = {};
    g_deskEdgePrimed = false;
    g_dishLastSent = {};
    g_logBaseline.clear();
    g_logPrimed = false;
}

}  // namespace coop::console_state_sync
