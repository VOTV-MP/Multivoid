// coop/dev/tower_drill_radio.cpp -- the tower drill's radio arms: the host's break, the client's route to the top of
// the tower's ladder, and the repair (radioreach, radio, radiored, radiojoin, radiojoinred); the reset lever's arm
// (radioreset) is tower_drill_radio_arms.cpp's. See coop/dev/tower_drill.h. The arm table and what the arms share
// are tower_drill.cpp's, tower_drill_detail.h and tower_drill_radio_steps.h.
//
// The BP fact the solver stands on (bp_cpp radiotower_C, line numbers are its pseudo-C++): a press of cell `i` runs
// switchNeighbors (:1414), which toggles the five cells (row + dr, col + dc) for (dr, dc) in (0,0), (-1,0), (1,0),
// (0,-1), (0,1), with row = i / puzzleSize.Y and col = i % puzzleSize.Y, each wrapped as
// ((row + dr + puzzleSize.X) % puzzleSize.X) * puzzleSize.Y + ((col + dc + puzzleSize.Y) % puzzleSize.Y)
// (:1430-:1470); each toggle is switchSwitch (:1345), so a neighbour the wrap names twice (a 2-wide torus) is
// toggled twice and ends unchanged.

#include "tower_drill_radio_steps.h"

#include "coop/dev/director/director.h"
#include "coop/dev/director/ladder_climb.h"
#include "coop/dev/director/routes.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/fuse_tower_ops.h"
#include "coop/world/fuse_tower_rows.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/radio_tower.h"
#include "ue_wrap/engine/engine.h"  // TryGetActorLocation

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The arms. Every client arm starts on one route: the client walks to the tower with the director, then to the foot
// of the tower's ladder, takes hold and climbs it through the game's own verb and key, so it ends at the top, within
// a use's reach of every part. radioreach prints the ladder's ends, whether it stood at the top, and how far it ended
// from each part a use reaches; a measurement, no verdict. radio: the host scrambles its tower until it reads
// broken; the client, at the top, reads the break and, when every part it uses is within the lane's reach, solves
// the puzzle with presses through the tower's own use and pulls the confirm lever; PASS when its copy and the
// host's last row read whole 20 s at most after the lever. radiored: radio with the client's gates open and no rows
// applied, which ends `radio FAIL: the host's tower stays broken`. A puzzle that is on (the game rule enableLightsOut)
// needs a press: a break solved with none is `radio FAIL: the puzzle needed no press`, and PASS prints lightsout=<0|1>.
// radiojoin: the host breaks its tower at its own world-ready, before any client joins; the first row the client's
// tower took must already read broken, and it then repairs as radio does; radiojoinred has the gates open, which ends
// `radiojoin FAIL: the host broke at <ms> but this copy never read it`. radioreset, radioresetred, radiofuse and
// radiofusered: at the top the client pulls the reset lever, or a blown fuse and inserts the good one the host hands it
// (tower_drill_radio_arms.cpp).

namespace coop::dev::tower_drill {
namespace {

using namespace detail;

namespace RT = ue_wrap::radio_tower;
namespace DR = coop::director;
namespace LD = coop::director::ladder;
using coop::net::FuseTowerRow;
using ue_wrap::FVector;

// ---- bounds: failure bounds of the instrument, never a wait for a milestone ----
constexpr int      kMaxScrambles = 20;       // the host's calls to break its tower, one a second
constexpr uint64_t kScrambleGapMs = 1000;
constexpr uint64_t kCrossMs = 30000;         // the host's break reaching this copy; a press answered
constexpr uint64_t kPressGapMs = 300;        // the least time between two presses
constexpr int      kWalkDeadlineS = 300;     // the director's own bound on the walk
constexpr float    kRingCm = 150.f;          // the standpoints about the confirm lever
constexpr int      kStandpoints = 8;
constexpr float    kStandReachCm = 60.f;     // a walk ends this near its standpoint, flat
constexpr float    kLadderRingCm = 80.f;     // about the ladder's foot; 80 + 25 stays inside the mount's 125 uu
constexpr float    kLadderReachCm = 25.f;    // a walk to the foot ends this near its standpoint, flat

// ---- the puzzle: the torus the radio tower's presses act on ----

// One press of cell `i` on the host's puzzle map, applied to `cells` (a W x H torus): the five toggles of
// switchNeighbors, each wrapped, a doubled toggle cancelling itself.
void Toggle(bool* cells, int width, int height, int i) {
    static const int kOffsets[5][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    const int row = i / height, col = i % height;
    for (const auto& o : kOffsets) {
        const int at = ((row + o[0] + width) % width) * height + ((col + o[1] + height) % height);
        cells[at] = !cells[at];
    }
}

// The presses that light every cell: the system over GF(2), one unknown a press, one equation a cell, solved by
// Gauss-Jordan elimination with the free unknowns left unpressed; the answer is checked by playing it.
bool SolveTorus(const RT::State& st, std::vector<int>& presses) {
    const int n = st.cellCount, width = st.width, height = st.height;
    if (n <= 0 || n > RT::kMaxCells || width <= 0 || height <= 0 || width * height != n) return false;
    uint32_t column[RT::kMaxCells] = {};  // bit c of column[j]: pressing j toggles cell c
    for (int j = 0; j < n; ++j) {
        bool v[RT::kMaxCells] = {};
        Toggle(v, width, height, j);
        for (int c = 0; c < n; ++c)
            if (v[c]) column[j] |= 1u << c;
    }
    uint64_t rows[RT::kMaxCells] = {};  // bit j: the unknown j; bit n: the right side, a cell that must flip
    for (int c = 0; c < n; ++c) {
        for (int j = 0; j < n; ++j)
            if ((column[j] >> c) & 1u) rows[c] |= 1ull << j;
        if (!st.cells[c]) rows[c] |= 1ull << n;
    }
    int pivotColumn[RT::kMaxCells] = {};
    int rank = 0;
    for (int col = 0; col < n; ++col) {
        int p = -1;
        for (int r = rank; r < n && p < 0; ++r)
            if ((rows[r] >> col) & 1ull) p = r;
        if (p < 0) continue;
        std::swap(rows[rank], rows[p]);
        for (int r = 0; r < n; ++r)
            if (r != rank && ((rows[r] >> col) & 1ull)) rows[r] ^= rows[rank];
        pivotColumn[rank++] = col;
    }
    for (int r = rank; r < n; ++r)
        if ((rows[r] >> n) & 1ull) return false;  // an equation with no unknown left and a cell to flip
    presses.clear();
    for (int r = 0; r < rank; ++r)
        if ((rows[r] >> n) & 1ull) presses.push_back(pivotColumn[r]);
    bool played[RT::kMaxCells] = {};
    for (int c = 0; c < n; ++c) played[c] = st.cells[c];
    for (int i : presses) Toggle(played, width, height, i);
    for (int c = 0; c < n; ++c)
        if (!played[c]) return false;
    return true;
}

bool AllLit(const RT::State& st) {
    if (st.cellCount == 0) return false;
    for (int i = 0; i < st.cellCount; ++i)
        if (!st.cells[i]) return false;
    return true;
}

// The client's gates stand open and it applies no rows (the controls), so the host's row is read as it is kept.
bool GatesOpen() {
    const Arm a = ArmOf();
    return a == Arm::RadioRed || a == Arm::RadioJoinRed || a == Arm::RadioResetRed || a == Arm::RadioFuseRed;
}

// The control whose copy is not asked to read the host's break: it reads the break from the kept row alone, and its
// own repair then runs on its own copy, which the lever's verdict tells from the host's.
bool BreakFromRowAlone() {
    const Arm a = ArmOf();
    return a == Arm::RadioRed || a == Arm::RadioResetRed || a == Arm::RadioFuseRed;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::RadioReach:    return "radioreach";
    case Arm::RadioRed:      return "radiored";
    case Arm::RadioReset:    return "radioreset";
    case Arm::RadioResetRed: return "radioresetred";
    case Arm::RadioFuse:     return "radiofuse";
    case Arm::RadioFuseRed:  return "radiofusered";
    case Arm::RadioJoin:     return "radiojoin";
    case Arm::RadioJoinRed:  return "radiojoinred";
    default:                 return "radio";
    }
}

// The name an arm's verdict lines carry: a control prints its green arm's name, so one fail marker serves both.
const char* VerdictName() {
    if (IsRadioReset(ArmOf())) return "radioreset";
    if (IsRadioFuse(ArmOf())) return "radiofuse";
    if (IsRadioJoin(ArmOf())) return "radiojoin";
    return "radio";
}

int BlownSlot(const FuseTowerRow& w) {
    for (int i = 0; i < w.fuseCount; ++i)
        if (w.fuses[i] == 2) return i;
    return -1;
}

bool FusesEqual(const RT::State& l, const FuseTowerRow& w) {
    if (l.fuseCount != w.fuseCount) return false;
    for (int i = 0; i < l.fuseCount; ++i)
        if (l.fuses[i] != w.fuses[i]) return false;
    return true;
}

// ---- the host: the break, on its own roll ---------------------------------------------------------------------

bool     g_hostSaid = false;
bool     g_hostBroken = false;
bool     g_hostBroke = false;      // the tower has read broken once: the drill calls no more rolls
bool     g_hostGaveUp = false;
int      g_hostScrambles = 0;
uint64_t g_hostCallMs = 0;
uint8_t  g_hostFuses[RT::kMaxFuses] = {};  // the fuse arms: the slots as last said
uint8_t  g_hostFuseCount = 0;

void HostTick(coop::net::Session& s) {
    void* t = RT::Instance();
    RT::State st;
    if (!t || !RT::Read(t, st) || st.fuseCount == 0 || st.cellCount == 0) return;
    if (!g_hostSaid || st.isBroken != g_hostBroken) {
        g_hostSaid = true;
        g_hostBroken = st.isBroken;
        UE_LOGI("[TOWER-DRILL] host radio broken=%d", st.isBroken ? 1 : 0);
    }
    if (IsRadioFuse(ArmOf())) {
        // The fuse arms want a fuse blown and the tower whole: one scramble blows the first good fuse
        // (radiotower.cpp:1496-1520); a scramble past the last good fuse breaks the tower, which they cannot use.
        if (g_hostFuseCount != st.fuseCount || std::memcmp(g_hostFuses, st.fuses, st.fuseCount) != 0) {
            g_hostFuseCount = st.fuseCount;
            std::memcpy(g_hostFuses, st.fuses, st.fuseCount);
            UE_LOGI("[TOWER-DRILL] host radio fuses=%s", Digits(st, true).c_str());
        }
        bool blown = false;
        for (int i = 0; i < st.fuseCount; ++i) blown = blown || st.fuses[i] == 2;
        if (blown && !st.isBroken) g_hostBroke = true;
        if (st.isBroken && !g_hostBroke && !g_hostGaveUp) {
            g_hostGaveUp = true;
            UE_LOGW("[TOWER-DRILL] host radio tower broke with no fuse left to blow first -- FAIL");
        }
    } else if (st.isBroken) {
        g_hostBroke = true;
    }
    if (ArmOf() == Arm::RadioReach || g_hostBroke || g_hostGaveUp) return;
    // The join arms break the tower at the host's own world-ready, before any client is in its world; the others
    // wait for a client's world.
    if (IsRadioJoin(ArmOf()) ? !coop::host_world_settle::Settled() : !s.IsSlotWorldReady(1)) return;
    if (g_hostScrambles >= kMaxScrambles) {
        g_hostGaveUp = true;
        UE_LOGW("[TOWER-DRILL] host radio could not break the tower in %d scrambles -- FAIL", kMaxScrambles);
        return;
    }
    const uint64_t now = NowMs();
    if (now - g_hostCallMs < kScrambleGapMs) return;
    g_hostCallMs = now;
    RT::Scramble(t);
    ++g_hostScrambles;
}

// ---- the client: walks to the tower, climbs to its parts, then repairs it ---------------------------------------

enum class Step : uint8_t { Arm, JoinCheck, Walk, LadderWalk, LadderClimb, Break, Press, Lever, Reset, Fuse, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_armMs = 0;              // the census: the first tick the tower and the host's row were both here
uint64_t g_rowBrokenMs = 0;        // the first tick the host's kept row read broken
bool     g_acted = false;
uint16_t g_actSeq = 0;
std::shared_ptr<DR::BackgroundWalk> g_walk;
bool     g_walkAsked = false;
float    g_routeUU = -1.f;
ue_wrap::CachedObjRef g_ladder;  // the tower's ladder, held across the climb's ticks
FVector  g_foot{}, g_top{};
bool     g_ladderWalkAsked = false;
bool     g_footReached = false;
LD::Climb g_climb;
int      g_fuseSlot = -1;          // the fuse arms: the host's blown slot this copy pulls
std::vector<int> g_presses;
size_t   g_pressNext = 0;
uint64_t g_leverMs = 0;
uint64_t g_pressMs = 0;            // when the last press was sent
uint32_t g_pressExpect = 0;        // the host's cells once the press now awaited is its own

void Go(Step s) {
    g_step = s;
    g_stepMs = NowMs();
    g_acted = false;
}

bool Overdue(uint64_t bound) { return NowMs() - g_stepMs > bound; }

// The DONE line, which every arm's end prints once: `what` is the arm and its verdict.
void SayDone(const char* what) {
    g_step = Step::Done;
    // An arm that ends while the climb still holds the forward key lets it go first.
    g_climb.Release(coop::players::Registry::Get().Local(), g_ladder.Get());
    const auto e = coop::fuse_tower_rows::AppliedEdges(coop::net::kFuseTowerKindRadio);
    UE_LOGI("[TOWER-DRILL] client DONE %s (ops sent %llu, claims unanswered %zu, rolls refused %llu; applied: "
            "breaks %u repairs %u presses %u pulls %u inserts %u levers %u fails %u)", what,
            static_cast<unsigned long long>(coop::fuse_tower_ops::OpsSent()), coop::fuse_tower_ops::PendingClaims(),
            static_cast<unsigned long long>(coop::fuse_tower_ops::RollsRefused()), e.breaks, e.repairs, e.presses,
            e.pulls, e.inserts, e.levers, e.fails);
}

// An arm's end: "<arm> <PASS|FAIL|ABANDONED>".
void Done(const char* verdict) {
    char what[64];
    std::snprintf(what, sizeof what, "%s %s", ArmName(), verdict);
    SayDone(what);
}

void Fail(const char* why) {
    UE_LOGW("[TOWER-DRILL] %s FAIL: %s", VerdictName(), why);
    Done("FAIL");
}

// A step the fixture could not give: the arm's name, then the DONE line.
void Abandon(const char* why) {
    UE_LOGW("[TOWER-DRILL] %s ABANDONED: %s", VerdictName(), why);
    Done("ABANDONED");
}

// The rows have answered my last op.
bool Answered() {
    return static_cast<int16_t>(static_cast<uint16_t>(coop::fuse_tower_ops::LastAck() - g_actSeq)) >= 0;
}

// A part of the tower and how far the walker stands from it; -1 while the part is not there.
struct PartDist {
    const char* name;
    RT::LookAt  at;
    float       uu = -1.f;
};

void Measure(void* tower, const FVector& me, PartDist& p) {
    FVector where{};
    p.uu = RT::PartLocation(tower, p.at, where) ? Dist(me, where) : -1.f;
}

// Where the client stands once the route ended: the line every radio arm prints, and for the repair arms the reach
// check of the parts they use. Returns false when the arm ended here.
bool AtTheParts(void* tower, void* player, bool reached) {
    FVector me{};
    if (!ue_wrap::engine::TryGetActorLocation(player, me)) {
        UE_LOGW("[TOWER-DRILL] %s: the walker's place does not read", ArmName());
        Done("ABANDONED");
        return false;
    }
    PartDist button0{"button0", {RT::Part::Button, 0}};
    PartDist confirm{"confirm lever", {RT::Part::Confirm, -1}};
    PartDist reset{"reset lever", {RT::Part::Reset, -1}};
    PartDist fuse0{"fuse0", {RT::Part::Fuse, 0}};
    for (PartDist* p : {&button0, &confirm, &reset, &fuse0}) Measure(tower, me, *p);
    UE_LOGI("[TOWER-DRILL] %s route=%.0f reached=%d dist button0=%.0f confirm=%.0f reset=%.0f fuse0=%.0f", ArmName(),
            g_routeUU, reached ? 1 : 0, button0.uu, confirm.uu, reset.uu, fuse0.uu);
    if (ArmOf() == Arm::RadioReach) {
        SayDone("radioreach");
        return false;
    }
    // Every part this client uses must be within the lane's reach, or the fixture, not the lane, is what ended it.
    std::vector<PartDist> used;
    if (IsRadioReset(ArmOf())) {
        used.push_back(reset);
    } else if (IsRadioFuse(ArmOf())) {
        PartDist p{"fuse slot", {RT::Part::Fuse, g_fuseSlot}};
        Measure(tower, me, p);
        used.push_back(p);
    } else {
        used.push_back(confirm);
        for (int i : g_presses) {
            PartDist p{"puzzle button", {RT::Part::Button, i}};
            Measure(tower, me, p);
            used.push_back(p);
        }
    }
    for (const PartDist& p : used)
        if (p.uu < 0.f || p.uu > coop::fuse_tower_ops::kTowerReachUU) {
            UE_LOGW("[TOWER-DRILL] %s ABANDONED: %s at %.0f after the climb", ArmName(), p.name, p.uu);
            Done("ABANDONED");
            return false;
        }
    return true;
}

// Where the tower and each part a use reaches stand, printed once before the walk is asked.
void LogParts(void* tower) {
    FVector root{};
    const bool haveRoot = ue_wrap::engine::TryGetActorLocation(tower, root);
    const RT::LookAt asks[] = {{RT::Part::Button, 0}, {RT::Part::Confirm, -1}, {RT::Part::Reset, -1}, {RT::Part::Fuse, 0}};
    char at[4][64];
    for (int i = 0; i < 4; ++i) {
        FVector v{};
        if (RT::PartLocation(tower, asks[i], v)) std::snprintf(at[i], sizeof at[i], "(%.0f,%.0f,%.0f)", v.X, v.Y, v.Z);
        else std::snprintf(at[i], sizeof at[i], "(unread)");
    }
    char rootAt[64];
    if (haveRoot) std::snprintf(rootAt, sizeof rootAt, "(%.0f,%.0f,%.0f)", root.X, root.Y, root.Z);
    else std::snprintf(rootAt, sizeof rootAt, "(unread)");
    UE_LOGI("[TOWER-DRILL] radio parts root=%s button0=%s confirm=%s reset=%s fuse0=%s", rootAt, at[0], at[1], at[2],
            at[3]);
}

// The standpoints about the lever with the ring at height `z`, and the route's trace of that try; printed counts.
std::vector<FVector> RingAtHeight(void* player, const FVector& lever, float z, std::string& trace) {
    trace.clear();
    std::vector<FVector> ring =
        DR::ReachableStandpoints(player, lever, kRingCm, kStandpoints, kStandReachCm, &z, &trace);
    UE_LOGI("[TOWER-DRILL] radio standpoints at z=%.0f: %zu", z, ring.size());
    return ring;
}

// The trace of a try that found nothing, one line per leg, capped.
void LogTrace(const std::string& trace) {
    constexpr int kMaxLines = 40;
    int printed = 0;
    size_t pos = 0;
    while (pos < trace.size() && printed < kMaxLines) {
        size_t nl = trace.find('\n', pos);
        if (nl == std::string::npos) nl = trace.size();
        UE_LOGI("[TOWER-DRILL] radio %s", trace.substr(pos, nl - pos).c_str());
        pos = nl + 1;
        ++printed;
    }
}

// The walk to the confirm lever's ring ended; every arm goes on to the tower's ladder, whatever the walk reached.
void WalkEnded(bool reached) {
    UE_LOGI("[TOWER-DRILL] %s base walk ended: route=%.0f reached=%d", ArmName(), g_routeUU, reached ? 1 : 0);
    Go(Step::LadderWalk);
}

// The ladder steps ended short. The reach arm prints the distances from where the client stands, then DONE; a
// repair arm ends ABANDONED, since the parts are out of reach.
void LadderAbandoned(void* tower, void* player, const char* why) {
    UE_LOGW("[TOWER-DRILL] %s ladder ABANDONED: %s", ArmName(), why);
    if (ArmOf() == Arm::RadioReach) AtTheParts(tower, player, g_footReached);
    else Done("ABANDONED");
}

// What the climb measured: the ladder's ends, whether it mounted, how long it climbed, where it ended.
void LadderReport(void* tower, void* player) {
    const LD::Outcome& o = g_climb.Result();
    UE_LOGI("[TOWER-DRILL] %s ladder foot=(%.0f,%.0f,%.0f) top=(%.0f,%.0f,%.0f) mounted=%d climbMs=%llu "
            "end=(%.0f,%.0f,%.0f) top=%d", ArmName(), g_foot.X, g_foot.Y, g_foot.Z, g_top.X, g_top.Y, g_top.Z,
            o.mounted ? 1 : 0,
            static_cast<unsigned long long>(o.climbMs), o.end.X, o.end.Y, o.end.Z, o.atTop ? 1 : 0);
    if (o.why) {
        LadderAbandoned(tower, player, o.why);
        return;
    }
    if (!o.atTop) {
        char why[96];
        std::snprintf(why, sizeof why, "the ladder let go %.0f uu from its top", Dist(o.end, g_top));
        LadderAbandoned(tower, player, why);
        return;
    }
    // At the top: the reach arm measures here; the others wait for the host's break to read on this copy.
    if (ArmOf() == Arm::RadioReach) AtTheParts(tower, player, g_footReached);
    else Go(Step::Break);
}

void ClientTick(void* player) {
    RadioView v;
    v.tower = RT::Instance();
    v.player = player;
    v.haveL = v.tower && RT::Read(v.tower, v.l);
    v.haveW = FindRow(coop::net::kFuseTowerKindRadio, 0, v.w);
    v.fuseSlot = g_fuseSlot;
    void* const tower = v.tower;
    const RT::State& l = v.l;
    const bool haveL = v.haveL;
    const FuseTowerRow& w = v.w;
    const bool haveW = v.haveW;
    // The host's break as this copy reads it: the host's row says broken and this copy shows its cells; the control
    // that applies no rows reads the row alone.
    const bool crossed = haveW && RowBroken(w) && (BreakFromRowAlone() || (haveL && l.isBroken && CellsEqual(l, w)));
    // The fuse arms' tower stays whole: the host's blown fuse as this copy reads it.
    const bool fuseCrossed = haveW && !RowBroken(w) && BlownSlot(w) >= 0 &&
                             (BreakFromRowAlone() || (haveL && !l.isBroken && FusesEqual(l, w)));
    if (haveW && RowBroken(w) && g_rowBrokenMs == 0) g_rowBrokenMs = NowMs();
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (g_stepMs == 0) g_stepMs = NowMs();
        if (!haveL || !haveW) {
            if (Overdue(kCrossMs)) {
                UE_LOGW("[TOWER-DRILL] %s ABANDONED: no radio tower here, or the host's rows never came", ArmName());
                Done("ABANDONED");
            }
            return;
        }
        FVector at{};
        ue_wrap::engine::TryGetActorLocation(tower, at);
        UE_LOGI("[TOWER-DRILL] client %s census: the radio tower at (%.0f, %.0f, %.0f), puzzle %dx%d, broken %d, "
                "fuses %s cells %s", ArmName(), at.X, at.Y, at.Z, l.width, l.height, l.isBroken ? 1 : 0,
                Digits(l, true).c_str(), Digits(l, false).c_str());
        g_armMs = NowMs();
        Go(IsRadioJoin(ArmOf()) ? Step::JoinCheck : Step::Walk);
        return;
    }
    case Step::JoinCheck: {
        // A joiner's tower reads the host's break as it arrives: the FIRST row it took says broken, before it walks
        // anywhere. The control applies no row, so it waits out the bound and says what it saw.
        uint8_t first = 0;
        if (!GatesOpen() && coop::fuse_tower_rows::FirstAppliedFlags(coop::net::kFuseTowerKindRadio, 0, first)) {
            const bool brokenOnArrival = (first & coop::net::kFuseTowerBroken) != 0;
            UE_LOGI("[TOWER-DRILL] client radiojoin: first row broken=%d", brokenOnArrival ? 1 : 0);
            if (!brokenOnArrival) {
                Abandon("the host's break was not in place on arrival (fixture)");
                return;
            }
            UE_LOGI("[TOWER-DRILL] client PASS the joiner's copy reads the host's break: first row broken=1, cells %s",
                    Digits(l, false).c_str());
            Go(Step::Walk);
            return;
        }
        if (!Overdue(kCrossMs)) return;
        if (!haveW || !RowBroken(w)) {
            Abandon("the host never broke its tower");
        } else if (GatesOpen()) {
            char why[96];
            std::snprintf(why, sizeof why, "the host broke at %llu ms but this copy never read it",
                          static_cast<unsigned long long>(g_rowBrokenMs > g_armMs ? g_rowBrokenMs - g_armMs : 0));
            Fail(why);
        } else {
            Fail("the host's break never reached this copy");
        }
        return;
    }
    case Step::Break: {
        // At the top of the ladder: the break is read here, the puzzle solved, the reach of the parts checked.
        const bool fuseArm = IsRadioFuse(ArmOf());
        if (fuseArm ? fuseCrossed : crossed) {
            const bool reset = IsRadioReset(ArmOf());
            if (fuseArm) {
                g_fuseSlot = BlownSlot(w);
                UE_LOGI("[TOWER-DRILL] client PASS the host's blown fuse reached this copy: slot %d, fuses %s",
                        g_fuseSlot, Digits(l, true).c_str());
                if (AtTheParts(tower, player, g_footReached)) Go(Step::Fuse);
                return;
            }
            if (!reset && (!haveL || !SolveTorus(l, g_presses))) {
                Abandon("no set of presses lights this puzzle (the fixture)");
                return;
            }
            // A puzzle that is on needs a press; a tower the drill broke with every cell lit is a fixture's fault.
            if (!reset && !l.skipPuzzle && g_presses.empty()) {
                Fail("the puzzle needed no press");
                return;
            }
            UE_LOGI("[TOWER-DRILL] client PASS the host's break reached this copy: cells %s, %zu presses solve it, "
                    "lightsout=%d", Digits(l, false).c_str(), g_presses.size(), l.skipPuzzle ? 0 : 1);
            if (AtTheParts(tower, player, g_footReached)) Go(reset ? Step::Reset : Step::Press);
            return;
        }
        if (Overdue(kCrossMs))
            Fail(fuseArm ? "the host's blown fuse never reached this copy" : "the host's break never reached this copy");
        return;
    }
    case Step::Walk: {
        if (!tower) return;
        if (!g_walkAsked) {
            // The walk is the director's, to a standpoint about the confirm lever that a route reaches (a part on a
            // platform ends no route of its own); a walk that gives up is a number, never a retry.
            FVector me{}, lever{};
            if (!ue_wrap::engine::TryGetActorLocation(player, me) ||
                !RT::PartLocation(tower, RT::LookAt{RT::Part::Confirm, -1}, lever)) {
                UE_LOGW("[TOWER-DRILL] %s ABANDONED: the walker's or the confirm lever's place does not read",
                        ArmName());
                Done("ABANDONED");
                return;
            }
            g_walkAsked = true;
            LogParts(tower);
            std::string trace;
            std::vector<FVector> ring = RingAtHeight(player, lever, lever.Z, trace);
            FVector root{};
            if (ring.empty() && ue_wrap::engine::TryGetActorLocation(tower, root))
                ring = RingAtHeight(player, lever, root.Z, trace);
            if (ring.empty()) {
                LogTrace(trace);
                WalkEnded(false);
                return;
            }
            float len = -1.f;
            if (!DR::RouteInLegs(player, me, ring[0], kStandReachCm, &len)) len = -1.f;
            g_routeUU = len;
            UE_LOGI("[TOWER-DRILL] %s walks to a standpoint %.0f uu from the confirm lever at (%.0f, %.0f, %.0f), "
                    "route %.0f uu", ArmName(), kRingCm, ring[0].X, ring[0].Y, ring[0].Z, len);
            g_walk = DR::StartBackgroundWalk(ring[0], kStandReachCm, kWalkDeadlineS);
            return;
        }
        if (!g_walk) return;
        const int state = g_walk->state.load();
        if (state == 0) return;
        g_walk.reset();
        WalkEnded(state == 1);
        return;
    }
    case Step::LadderWalk: {
        if (!tower) return;
        if (!g_ladderWalkAsked) {
            // The walk is the director's, to a standpoint about the ladder's foot at the foot's height; the mount
            // takes the closest point of the ladder within 125 uu of the walker.
            g_ladderWalkAsked = true;
            g_ladder.Set(LD::Find(tower));
            FVector me{};
            if (!g_ladder.Get() || !LD::Foot(g_ladder.Get(), g_foot) || !LD::Top(g_ladder.Get(), g_top) ||
                !ue_wrap::engine::TryGetActorLocation(player, me)) {
                LadderAbandoned(tower, player, "the tower's ladder or the walker's place does not read");
                return;
            }
            std::string trace;
            const float z = g_foot.Z;
            const std::vector<FVector> ring =
                DR::ReachableStandpoints(player, g_foot, kLadderRingCm, kStandpoints, kLadderReachCm, &z, &trace);
            UE_LOGI("[TOWER-DRILL] %s ladder standpoints about the foot (%.0f, %.0f, %.0f): %zu", ArmName(), g_foot.X,
                    g_foot.Y, g_foot.Z, ring.size());
            if (ring.empty()) {
                LogTrace(trace);
                LadderAbandoned(tower, player, "no route reaches a standpoint about the ladder's foot");
                return;
            }
            g_walk = DR::StartBackgroundWalk(ring[0], kLadderReachCm, kWalkDeadlineS);
            return;
        }
        if (!g_walk) return;
        const int state = g_walk->state.load();
        if (state == 0) return;
        g_walk.reset();
        g_footReached = state == 1;
        FVector me{};
        const bool read = ue_wrap::engine::TryGetActorLocation(player, me);
        UE_LOGI("[TOWER-DRILL] %s ladder foot walk ended: reached=%d, the walker stands %.0f uu from the foot",
                ArmName(), g_footReached ? 1 : 0, read ? Dist(me, g_foot) : -1.f);
        if (!g_footReached) {
            LadderAbandoned(tower, player, "the walk to the ladder's foot ended short");
            return;
        }
        Go(Step::LadderClimb);
        return;
    }
    case Step::LadderClimb:
        if (g_climb.Tick(player, g_ladder.Get())) LadderReport(tower, player);
        return;
    case Step::Press: {
        if (!haveL || !haveW) return;
        if (g_pressNext < g_presses.size()) {
            if (!g_acted) {
                if (NowMs() - g_pressMs < kPressGapMs) return;
                // What the host's cells read once this press is its own: the press is the host's, so its effect
                // arrives in the rows and nowhere else.
                bool cells[RT::kMaxCells] = {};
                for (int i = 0; i < w.lightCount && i < RT::kMaxCells; ++i) cells[i] = ((w.lights >> i) & 1u) != 0;
                Toggle(cells, l.width, l.height, g_presses[g_pressNext]);
                g_pressExpect = 0;
                for (int i = 0; i < w.lightCount && i < RT::kMaxCells; ++i)
                    if (cells[i]) g_pressExpect |= 1u << i;
                const uint64_t before = coop::fuse_tower_ops::OpsSent();
                RT::Use(tower, player, RT::LookAt{RT::Part::Button, g_presses[g_pressNext]});
                if (!GatesOpen() && coop::fuse_tower_ops::OpsSent() == before) {
                    Fail("a puzzle press was not sent to the host");
                    return;
                }
                g_actSeq = coop::fuse_tower_ops::LastSentSeq();
                g_acted = true;
                g_stepMs = g_pressMs = NowMs();
                return;
            }
            if (GatesOpen() || (Answered() && w.lights == g_pressExpect)) {
                ++g_pressNext;
                g_acted = false;
            } else if (Overdue(kCrossMs)) {
                Fail(Answered() ? "the host's row never showed a press" : "the host never answered a press");
            }
            return;
        }
        // Every press shown: the host's rows must have lit this copy's puzzle before the lever is pulled.
        if (!GatesOpen() && !AllLit(l)) {
            if (Overdue(kCrossMs)) Fail("the host's presses never lit this copy's puzzle");
            return;
        }
        UE_LOGI("[TOWER-DRILL] client pressed %zu puzzle buttons, this copy's cells read %s", g_presses.size(),
                Digits(l, false).c_str());
        Go(Step::Lever);
        return;
    }
    case Step::Lever: {
        if (!haveL) return;
        if (!g_acted) {
            const uint64_t before = coop::fuse_tower_ops::OpsSent();
            RT::Use(tower, player, RT::LookAt{RT::Part::Confirm, -1});
            if (!GatesOpen() && coop::fuse_tower_ops::OpsSent() == before) {
                Fail("the confirm lever's press was not sent to the host");
                return;
            }
            g_actSeq = coop::fuse_tower_ops::LastSentSeq();
            g_acted = true;
            g_leverMs = NowMs();
            UE_LOGI("[TOWER-DRILL] client pulled the confirm lever (seq %u)", g_actSeq);
            return;
        }
        const bool copyWhole = !l.isBroken;
        const bool hostWhole = haveW && !RowBroken(w);
        if (copyWhole && hostWhole) {
            UE_LOGI("[TOWER-DRILL] %s PASS (presses=%zu, climbMs=%llu, lightsout=%d)", VerdictName(), g_presses.size(),
                    static_cast<unsigned long long>(g_climb.Result().climbMs), l.skipPuzzle ? 0 : 1);
            Done("PASS");
            return;
        }
        if (NowMs() - g_leverMs > kRadioVerdictMs)
            Fail(!hostWhole && !copyWhole ? "the host's tower stays broken and this copy reads broken too"
                 : !hostWhole             ? "the host's tower stays broken"
                                          : "this copy still reads broken");
        return;
    }
    case Step::Fuse:
        FuseArmTick(v);
        return;
    case Step::Reset:
        ResetArmTick(v);
        return;
    case Step::Done:
        return;
    }
}

}  // namespace

namespace detail {

bool CellsEqual(const RT::State& l, const FuseTowerRow& w) {
    if (l.cellCount != w.lightCount) return false;
    for (int i = 0; i < l.cellCount; ++i)
        if (l.cells[i] != (((w.lights >> i) & 1u) != 0)) return false;
    return true;
}

std::string Digits(const RT::State& st, bool fuses) {
    std::string s;
    const int n = fuses ? st.fuseCount : st.cellCount;
    for (int i = 0; i < n; ++i) s += fuses ? static_cast<char>('0' + st.fuses[i] % 10) : (st.cells[i] ? '1' : '0');
    return s;
}

std::string RowCells(const FuseTowerRow& w) {
    std::string s;
    for (int i = 0; i < w.lightCount; ++i) s += ((w.lights >> i) & 1u) ? '1' : '0';
    return s;
}

void RadioDone(const char* verdict) { Done(verdict); }
void RadioFail(const char* why) { Fail(why); }
void RadioAbandon(const char* why) { Abandon(why); }

void RadioInconclusive(const char* why) {
    UE_LOGW("[TOWER-DRILL] %s INCONCLUSIVE: %s", VerdictName(), why);
    Done("INCONCLUSIVE");
}

void RadioHostTick(coop::net::Session& s) {
    HostTick(s);
    FuseArmHostTick();
}

void RadioClientTick() {
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void RadioReset() {
    g_hostSaid = g_hostBroken = g_hostBroke = g_hostGaveUp = false;
    g_hostScrambles = 0;
    g_hostCallMs = 0;
    g_hostFuseCount = 0;
    if (g_walk) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_walkAsked = false;
    g_routeUU = -1.f;
    // A climb the session ended lets go of the forward key before it is forgotten.
    g_climb.Release(coop::players::Registry::Get().Local(), g_ladder.Get());
    g_ladder.Reset();
    g_foot = g_top = FVector{};
    g_ladderWalkAsked = false;
    g_footReached = false;
    g_climb = LD::Climb{};
    g_presses.clear();
    g_pressNext = 0;
    g_step = Step::Arm;
    g_stepMs = 0;
    g_armMs = g_rowBrokenMs = 0;
    g_fuseSlot = -1;
    g_acted = false;
    g_actSeq = 0;
    g_leverMs = 0;
    g_pressMs = 0;
    g_pressExpect = 0;
    ResetArmReset();
    FuseArmReset();
}

}  // namespace detail
}  // namespace coop::dev::tower_drill
