// coop/interactables/console_state_sync.cpp -- see coop/interactables/console_state_sync.h.

#include "coop/interactables/console_state_sync.h"

#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/net/session.h"
#include "coop/interactables/signal_catch_sync.h"
#include "coop/dev/desk_diag.h"  // the join-adopt baseline hook, a no-op unless enabled

#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/coords_panel.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/space_renderer.h"

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
namespace CP = ue_wrap::coords_panel;

using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

constexpr auto kSkyPoll       = std::chrono::milliseconds(1000);  // host set poll + client sweep
constexpr auto kDeskPoll      = std::chrono::milliseconds(1000);  // the desk log producer's cadence
constexpr auto kDishInterval  = std::chrono::milliseconds(330);   // ~3 Hz claimed stream
constexpr int  kMaxSkyRows    = 15;                               // 5 parts, above the native set's size
const wchar_t* const kDeskClaim = L"desk";

// Sky-signal state. g_skyMirror: on the host the last broadcast set; on the client the last
// wire-applied set, the mirror the sweep and the catch detector compare against.
std::vector<SR::SignalRow> g_skyMirror;
bool g_haveWireSet = false;          // client: first snapshot landed
uint8_t g_skyGen = 0;                // host: snapshot generation
void* g_suppressedOn = nullptr;      // client: the instance whose roller we killed
Clock::time_point g_nextSkyPoll{};

// Client part assembly: a snapshot arrives as up to 5 parts of up to 3 rows. `started` bounds a
// stale half-assembly's lifetime: an abandoned assembly whose generation byte wraps into a much
// later snapshot's would otherwise contribute leftover rows, so anything older than 5 s is reset
// before the new part is taken.
struct PartAssembly {
    uint8_t gen = 0, parts = 0, got = 0;
    bool present[5] = {};
    std::vector<SR::SignalRow> rows;
    Clock::time_point started{};
};
PartAssembly g_assembly;
constexpr auto kAssemblyTTL = std::chrono::seconds(5);

// Desk state is adopt-only, the joiner seed: live desk input rides the field-granular DeskInput
// lane and the simulation outputs ride DeskSimPose. The desk poll timer is the log producer's
// cadence.
Clock::time_point g_nextDeskPoll{};

// Desk log lines. The producer's diff baseline: the whole coord log text (the blueprint caps it
// at 1000 chars) at the last poll or apply. First sight primes without replaying history, so a
// session-old terminal does not flood the wire at install.
std::wstring g_logBaseline;
bool g_logPrimed = false;
// The blueprint cap is 1000 chars; read a little over so the window never truncates.
constexpr size_t kLogReadMax = 1100;

// Dish aim.
CP::DishAim g_dishLastSent;
Clock::time_point g_nextDish{};

bool RowIdentityEq(const SR::SignalRow& a, const SR::SignalRow& b) {
    // Exact float equality is right here: every non-host copy of a row was byte-copied off the wire
    // from the host's roll, so identities compare bit-identical.
    return a.x == b.x && a.y == b.y && a.z == b.z && a.frequency == b.frequency;
}

bool RowContentEq(const SR::SignalRow& a, const SR::SignalRow& b) {
    // Alpha and the lifetimes are excluded: they tick every frame, and comparing them would turn
    // the 1 Hz host poll into an unconditional broadcast.
    return RowIdentityEq(a, b) && a.type == b.type && a.strength == b.strength &&
           a.frequencySpread == b.frequencySpread && a.polarity == b.polarity &&
           a.polaritySpread == b.polaritySpread && a.direction == b.direction &&
           a.objectName == b.objectName;
}

bool SetContentEq(const std::vector<SR::SignalRow>& a, const std::vector<SR::SignalRow>& b) {
    if (a.size() != b.size()) return false;
    // Order-insensitive: deleteSignal compacts the arrays, so index order can differ between polls
    // without a real change. Quadratic over a dozen rows.
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

// Send the full current set as parts. `toSlot` < 0 broadcasts; otherwise point-to-point, the
// connect snapshot.
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
    // coordIsPing is never adopted: it is the ping machine's run flag, and adopting true would wake
    // a phantom simulation on the joiner. The wire field stays as diagnostic truth; a joiner's own
    // desk cannot be mid-ping.
    sc.coordIsPing = false;
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

// The producer diff. The terminal text is one local monotone sequence (writeToCoordLog_2
// appends and trims to the last 1000 chars), so the largest suffix of the baseline that
// prefixes the current text recovers the appended text exactly on the producer. A self-similar
// repeated line can over-match the overlap and eat its own duplicate; only the animated bar
// lines repeat exactly, and those are filtered below. Only complete event lines ride the wire.

// Only the CDOWN family is filtered: it is the one animated family every peer regenerates
// itself (the cooldown mirrors through DeskInput charge events and the native per-peer decay,
// and each peer's own coord-log pump rewrites its CDOWN lines), so shipping them would
// double-write terminals. The other event families are gated on the presser's own inputs or on
// the ping machine, which never run on a mirror, so they ship as normal producer lines; their
// volume is bounded by the 1000-char cap at the 1 Hz producer.
bool IsAnimatedLogLine(const std::wstring& line) {
    static const wchar_t* const kPrefixes[] = {
        L"CDOWN: [",
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

// Poll the terminal and ship the new complete event lines: at 1 Hz from the desk poll, and
// before a wire apply, so the apply's baseline advance cannot swallow a pending local line.
void ProduceLogLines(coop::net::Session* s) {
    // Steady state, the log unchanged on almost every poll: one in-place compare against the live
    // engine string with no allocation; the wstring build below runs only on change.
    if (g_logPrimed && CD::CoordLogTailEquals(g_logBaseline, kLogReadMax)) return;
    const std::wstring cur = CD::ReadCoordLogTail(kLogReadMax);
    if (!g_logPrimed) {
        // First sight: prime without replaying history. The terminal is session-local even in
        // single player, so a joiner starts empty.
        g_logBaseline = cur;
        g_logPrimed = true;
        return;
    }
    if (cur == g_logBaseline) return;
    // The largest suffix of the baseline that prefixes cur is the shared region.
    size_t k = g_logBaseline.size() < cur.size() ? g_logBaseline.size() : cur.size();
    for (; k > 0; --k) {
        if (cur.compare(0, k, g_logBaseline, g_logBaseline.size() - k, k) == 0) break;
    }
    std::wstring fresh = cur.substr(k);
    g_logBaseline = cur;
    // Split on CRLF; writeToCoordLog_2 appends line plus CRLF atomically on the game thread, so the
    // fresh region is always whole lines.
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

    // Sky signals.
    if (srUp && now >= g_nextSkyPoll) {
        g_nextSkyPoll = now + kSkyPoll;
        if (isHost) {
            // The authority: broadcast the set on any content change.
            std::vector<SR::SignalRow> cur;
            if (SR::ReadSignals(cur) && s->connected() && !SetContentEq(cur, g_skyMirror)) {
                ++g_skyGen;
                SendSkySnapshot(s, cur, g_skyGen, -1);
                g_skyMirror = std::move(cur);
                UE_LOGI("console_state: sky snapshot gen=%u broadcast (%zu row(s))",
                        static_cast<unsigned>(g_skyGen), g_skyMirror.size());
            }
        } else {
            // Client: enforce the roller suppression per instance; a level reload spawns a fresh
            // spaceRenderer with a fresh armed timer.
            void* inst = SR::Instance();
            if (inst && inst != g_suppressedOn && SR::KillClientSpawnTimer()) {
                g_suppressedOn = inst;
                UE_LOGI("console_state: client sky roller timer killed (instance %p)", inst);
            }
            if (g_haveWireSet) {
                std::vector<SR::SignalRow> cur;
                if (SR::ReadSignals(cur)) {
                    // The divergence sweep: a local row the wire never expressed (a pre-kill
                    // self-roll, a kill miss) is deleted, so the client set stays a pure mirror.
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

    // The desk log producer, 1 Hz. Every peer produces, since event lines originate where the
    // action ran; the CDOWN filter keeps the one mirror-regenerated family off the wire.
    if (cdUp && now >= g_nextDeskPoll) {
        g_nextDeskPoll = now + kDeskPoll;
        ProduceLogLines(s);
    }
    if (cdUp && coop::device_occupancy::LocalHolds(kDeskClaim) && now >= g_nextDish) {
        g_nextDish = now + kDishInterval;
        // A field-compare dedupe, not memcmp: DishAim has tail padding after the direction byte,
        // and indeterminate stack padding would defeat the dedupe and turn the poll into an
        // unconditional stream. The live cursor is not on this lane (it streams on the unreliable
        // DeskCursorPose lane and is interpolated on the mirror); this lane carries only the
        // discrete committed locks, so a cursor sweep never triggers a reliable re-send, only a
        // commit does.
        auto aimEq = [](const CP::DishAim& a, const CP::DishAim& b) {
            return a.c0X == b.c0X && a.c0Y == b.c0Y &&
                   a.c1X == b.c1X && a.c1Y == b.c1Y &&
                   a.c2X == b.c2X && a.c2Y == b.c2Y &&
                   a.selected == b.selected && a.direction == b.direction;
        };
        CP::DishAim aim;
        if (CP::ReadDishAim(aim) && !aimEq(aim, g_dishLastSent)) {
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

    // Snapshot complete: reconcile the local set to it.
    if (!SR::EnsureResolved() || !SR::Instance()) {
        // World not up yet; defensive.
        g_assembly = {};
        return;
    }
    // An in-flight local catch must outrank a stale snapshot row: signal_catch_sync runs its
    // detector now and strips recently-caught identities from the incoming set.
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
    // Adopt-only: desk input rides DeskInput and the outputs ride DeskSimPose, so a live DeskState
    // does not exist on the wire and anything but an adopt is dropped.
    if (!p.adopt) return;
    if (senderSlot != 0) return;  // adopt snapshots are host-only
    // Finite-validate before any engine write; a NaN filter offset or cooldown must never land in
    // blueprint floats.
    const float vals[] = { p.dlPoFilterOffset, p.dlFrFilterOffset, p.dlPoFilterSpeed,
                           p.dlFrFilterSpeed, p.dlDownloading, p.dlResDetecPercent,
                           p.coordCooldown };
    for (float v : vals)
        if (!std::isfinite(v)) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;

    // Dev: capture the client's pre-adopt local scalars before the host seed overwrites them.
    if (s->role() == coop::net::Role::Client)
        coop::dev::desk_diag::NoteJoinAdopt();

    const CD::Scalars sc = PayloadToScalars(p);
    if (CD::WriteScalars(sc)) {
        // Prime the input lane's poll baselines, so the seeded values never read as local edges.
        coop::desk_input_sync::PrimeBaselines();
    }
}

void OnDishAim(const coop::net::DishAimStatePayload& p, uint8_t senderSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // Committed coords only; the live cursor lives on the DeskCursorPose lane.
    const float vals[] = { p.c0X, p.c0Y, p.c1X, p.c1Y, p.c2X, p.c2Y };
    for (float v : vals)
        if (!std::isfinite(v)) return;
    // Only the desk-claim holder streams aim; drop anything else.
    const uint8_t holder = coop::device_occupancy::HolderOf(kDeskClaim);
    if (holder == 0xFF || senderSlot != holder) return;
    if (coop::device_occupancy::LocalHolds(kDeskClaim)) return;  // we are the streamer
    if (!CD::EnsureResolved()) return;
    CP::DishAim aim;  // viewX/viewY stay 0; the committed write ignores them
    aim.c0X = p.c0X; aim.c0Y = p.c0Y;
    aim.c1X = p.c1X; aim.c1Y = p.c1Y;
    aim.c2X = p.c2X; aim.c2Y = p.c2Y;
    aim.selected = p.selected;
    aim.direction = p.direction;
    CP::WriteDishCommitted(aim);
}

void OnDeskLogLine(const coop::net::DeskLogLinePayload& p, uint8_t senderSlot) {
    (void)senderSlot;  // producer-symmetric; the host relay already fanned out
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    const size_t n = p.len <= sizeof(p.line) ? p.len : sizeof(p.line);
    if (n == 0) return;
    if (!CD::EnsureResolved() || !CD::Instance()) return;
    // Flush our pending local lines first: the apply below advances the producer baseline past the
    // whole terminal, which would otherwise swallow an unshipped local line from the last poll
    // window.
    ProduceLogLines(s);
    std::wstring line(p.line, p.line + n);
    CD::AppendCoordLog(line);  // writeToCoordLog_2: native CRLF + repaint + cap
    // Echo-proof: advance our own diff baseline past the applied text.
    g_logBaseline = CD::ReadCoordLogTail(kLogReadMax);
    g_logPrimed = true;
}

void QueueConnectBroadcastForSlot(int peerSlot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    // Sky set: the joiner starts from the authoritative snapshot; its own save-loaded world rolled
    // nothing, since signals are runtime-only.
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
    // Desk scalars: the adopt snapshot trues up the live scalars since the transferred save. The
    // joiner's terminal history is not replayed; the coord log is session-local even in single
    // player, since the panel save data carries no log field.
    if (CD::EnsureResolved() && CD::Instance()) {
        CD::Scalars cur;
        if (CD::ReadScalars(cur)) {
            SendDeskState(s, cur, 1, peerSlot);
        }
        // The committed locks are change-gated, so an idle occupant sends nothing and a joiner
        // would see stale locks; snapshot the committed coords on connect. The live cursor needs
        // none, since its stream re-primes it within one send interval.
        CP::DishAim aim;
        if (CP::ReadDishAim(aim)) {
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
    // Client: restore the native sky roller (one reflected spawnSignal re-arms the blueprint's own
    // loop), so the world returns to single-player behaviour.
    auto* s = g_session.load(std::memory_order_acquire);
    if (s && s->role() == coop::net::Role::Client && g_suppressedOn) {
        if (SR::RestoreRoller())
            UE_LOGI("console_state: client sky roller restored (native loop re-armed)");
    }
    g_suppressedOn = nullptr;
    g_skyMirror.clear();
    g_haveWireSet = false;
    g_assembly = {};
    g_dishLastSent = {};
    g_logBaseline.clear();
    g_logPrimed = false;
}

}  // namespace coop::console_state_sync
