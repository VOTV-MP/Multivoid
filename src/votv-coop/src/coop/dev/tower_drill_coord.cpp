// coop/dev/tower_drill_coord.cpp -- the tower drill's coordinate arms (run, join and their controls); see
// coop/dev/tower_drill.h. The arm table and what the arms share are tower_drill.cpp's and tower_drill_detail.h.

#include "tower_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/fuse_tower_ops.h"
#include "coop/world/fuse_tower_rows.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/coord_tower.h"
#include "ue_wrap/engine/engine.h"             // TryGetActorLocation
#include "ue_wrap/engine/engine_mainplayer.h"  // what the hand holds

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::tower_drill {
namespace {

using namespace detail;

namespace CT = ue_wrap::coord_tower;
using coop::net::FuseTowerRow;
using ue_wrap::FVector;

// Where the rig client's profile stands: the alpha base spot the rig's pose tool writes.
constexpr FVector  kBase{-1941.f, 264.f, 6318.f};
constexpr int      kMaxScrambles = 12;  // four fuses, a grace and the break, twice over
// Failure bounds only: each leg ends on the state it waits for.
constexpr uint64_t kCrossMs = 30000;
constexpr uint64_t kRepairMs = 120000;  // the host's whole repair: a pull, an insert, the presses, the lever's run

void* DrillTower() {
    void* towers[coop::net::kFuseTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kFuseTowers);
    void* best = nullptr;
    float bestD = 0.f;
    for (int32_t i = 0; i < n; ++i) {
        FVector p{};
        if (!ue_wrap::engine::TryGetActorLocation(towers[i], p)) continue;
        const float d = Dist(p, kBase);
        if (!best || d < bestD) { best = towers[i]; bestD = d; }
    }
    return best;
}

std::string Digits(const CT::State& st, bool fuses) {
    std::string s;
    const int n = fuses ? st.fuseCount : st.lightCount;
    for (int i = 0; i < n; ++i) s += fuses ? static_cast<char>('0' + st.fuses[i] % 10) : (st.lights[i] ? '1' : '0');
    return s;
}

std::string RowDigits(const FuseTowerRow& w, bool fuses) {
    std::string s;
    const int n = fuses ? w.fuseCount : w.lightCount;
    for (int i = 0; i < n; ++i)
        s += fuses ? static_cast<char>('0' + w.fuses[i] % 10) : (((w.lights >> i) & 1u) ? '1' : '0');
    return s;
}

bool HostRow(int32_t id, FuseTowerRow& out) { return FindRow(coop::net::kFuseTowerKindCoordinate, id, out); }

// Whether this copy of a tower reads the host's break, fuses and puzzle.
bool SameState(const CT::State& l, const FuseTowerRow& w) {
    return l.isBroken == RowBroken(w) && Digits(l, true) == RowDigits(w, true) &&
           Digits(l, false) == RowDigits(w, false);
}

void Census(const char* side) {
    void* towers[coop::net::kFuseTowers];
    const int32_t n = CT::ReadAll(towers, coop::net::kFuseTowers);
    for (int32_t i = 0; i < n; ++i) {
        CT::State st;
        FVector p{};
        if (!CT::Read(towers[i], st) || !ue_wrap::engine::TryGetActorLocation(towers[i], p)) continue;
        UE_LOGI("[TOWER-DRILL] %s census tower %d at (%.0f, %.0f, %.0f), %.0f m from the base: broken %d opened %d "
                "lever %s fuses %s lights %s", side, st.id, p.X, p.Y, p.Z, Dist(p, kBase) / 100.f,
                st.isBroken ? 1 : 0, st.opened ? 1 : 0, st.leverUp ? "up" : "down", Digits(st, true).c_str(),
                Digits(st, false).c_str());
    }
}

// The presses that light every light: a button toggles itself and its ring neighbours (switchNeighbors), presses
// commute, and a scramble is a set of presses, so a set that undoes it exists. Brute force over n <= 16.
bool Solve(const CT::State& st, std::vector<int>& presses) {
    const int n = st.lightCount;
    for (uint32_t mask = 0; mask < (1u << n); ++mask) {
        bool l[CT::kMaxLights];
        for (int i = 0; i < n; ++i) l[i] = st.lights[i];
        for (int i = 0; i < n; ++i) {
            if (!(mask & (1u << i))) continue;
            l[i] = !l[i];
            l[(i + n + 1) % n] = !l[(i + n + 1) % n];
            l[(i + n - 1) % n] = !l[(i + n - 1) % n];
        }
        bool all = true;
        for (int i = 0; i < n && all; ++i) all = l[i];
        if (!all) continue;
        presses.clear();
        for (int i = 0; i < n; ++i)
            if (mask & (1u << i)) presses.push_back(i);
        return true;
    }
    return false;
}

int BlownSlot(const CT::State& st) {
    for (int i = 0; i < st.fuseCount; ++i)
        if (st.fuses[i] == 2) return i;
    return -1;
}

// ---- the host: the break, then its own player's repair ---------------------------------------------------------

enum class HostStep : uint8_t { Arm, Open, AwaitFar, Pull, Insert, Unsolve, FailLever, Solve, Lever, Done };
HostStep  g_host = HostStep::Arm;
uint64_t  g_hostMs = 0;
void*     g_hostTower = nullptr;
int32_t   g_hostSlot = -1;
bool      g_hostActed = false;
CT::State g_hostLast;

void HostGo(HostStep s) {
    g_host = s;
    g_hostMs = NowMs();
    g_hostActed = false;
}

void HostSays(const char* what, const CT::State& st) {
    UE_LOGI("[TOWER-DRILL] host %s: tower %d broken %d opened %d lever %s fuses %s lights %s", what, st.id,
            st.isBroken ? 1 : 0, st.opened ? 1 : 0, st.leverUp ? "up" : "down", Digits(st, true).c_str(),
            Digits(st, false).c_str());
}

void HostTick(coop::net::Session& s) {
    if (g_host == HostStep::Done) return;
    void* me = coop::players::Registry::Get().Local();
    CT::State st;
    if (g_host == HostStep::Arm) {
        void* t = DrillTower();
        if (!t || !me || (IsRun(ArmOf()) && !s.IsSlotWorldReady(1))) return;
        Census("host");
        int n = 0;
        while (n < kMaxScrambles && CT::Read(t, st) && !st.isBroken) {
            CT::Scramble(t);
            ++n;
        }
        CT::Read(t, st);
        char what[64];
        std::snprintf(what, sizeof(what), "BROKE the drill's tower after %d scrambles", n);
        HostSays(st.isBroken ? what : "could not break the drill's tower -- FAIL", st);
        g_hostTower = t;
        HostGo(IsRun(ArmOf()) ? HostStep::Open : HostStep::Done);
        return;
    }
    if (!me || !g_hostTower || !CT::Read(g_hostTower, st)) return;
    const bool overdue = NowMs() - g_hostMs > kCrossMs;
    switch (g_host) {
    case HostStep::Open:
        if (st.opened && !st.isAnim) { HostSays("opened its panel", st); HostGo(HostStep::AwaitFar); return; }
        if (!g_hostActed) { g_hostActed = true; CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Retract, -1}); }
        if (overdue) { HostSays("its panel never opened -- FAIL", st); HostGo(HostStep::Done); }
        return;
    case HostStep::AwaitFar:
        // The client's two acts from out of reach come first: a press and a pull, both refused here.
        if (coop::fuse_tower_ops::OpsRefused() >= 2) { HostSays("refused the client's two far acts", st); HostGo(HostStep::Pull); return; }
        if (NowMs() - g_hostMs > 3 * kCrossMs) { HostSays("the client's far acts never came; repairing anyway", st); HostGo(HostStep::Pull); }
        return;
    case HostStep::Pull:
        if (!g_hostActed) {
            g_hostSlot = BlownSlot(st);
            if (g_hostSlot < 0) { HostSays("no blown fuse to pull -- FAIL", st); HostGo(HostStep::Done); return; }
            g_hostActed = true;
            CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Fuse, g_hostSlot});
            return;
        }
        if (st.fuses[g_hostSlot] == 0) { HostSays("pulled a blown fuse", st); HostGo(HostStep::Insert); return; }
        if (overdue) { HostSays("its pull never emptied the slot -- FAIL", st); HostGo(HostStep::Done); }
        return;
    case HostStep::Insert:
        if (!g_hostActed) {
            FVector at{};
            if (!ue_wrap::engine::TryGetActorLocation(me, at)) return;
            at.Z += 50.f;
            void* fuse = CT::SpawnFuse(at);
            if (!fuse) { HostSays("could not spawn a fuse -- FAIL", st); HostGo(HostStep::Done); return; }
            g_hostActed = true;
            CT::InsertFuse(g_hostTower, me, g_hostSlot, fuse);
            return;
        }
        if (st.fuses[g_hostSlot] == 1) { HostSays("inserted a good fuse", st); HostGo(st.isBroken ? HostStep::Unsolve : HostStep::Done); return; }
        if (overdue) { HostSays("its insert never filled the slot -- FAIL", st); HostGo(HostStep::Done); }
        return;
    case HostStep::Unsolve:
        // A scramble can leave every light lit (its picks cancel); one press leaves the puzzle to solve.
        if (!g_hostActed) {
            g_hostActed = true;
            CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Button, 0});
            return;
        }
        HostSays("pressed puzzle button 0", st);
        HostGo(HostStep::FailLever);
        return;
    case HostStep::FailLever:
        // The lever on an unsolved puzzle: its judgement fails and takes the lever back down.
        if (!g_hostActed) {
            g_hostActed = true;
            CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Lever, -1});
            return;
        }
        // The lever runs as the press returns, so its rest down again is the judgement's fail branch run through.
        if (!st.leverUp && !st.leverMoving && st.isBroken) {
            HostSays("pulled the lever on the unsolved puzzle, which failed", st);
            HostGo(HostStep::Solve);
            return;
        }
        if (overdue) { HostSays("its failed lever never came back down -- FAIL", st); HostGo(HostStep::Done); }
        return;
    case HostStep::Solve: {
        bool lit = st.lightCount > 0;
        for (int i = 0; i < st.lightCount; ++i) lit = lit && st.lights[i];
        if (lit) { HostSays("solved the puzzle", st); HostGo(HostStep::Lever); return; }
        if (!g_hostActed) {
            std::vector<int> presses;
            if (!Solve(st, presses)) { HostSays("no set of presses lights this puzzle -- FAIL", st); HostGo(HostStep::Done); return; }
            g_hostActed = true;
            for (int i : presses) CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Button, i});
            return;
        }
        if (overdue) { HostSays("its presses never lit the puzzle -- FAIL", st); HostGo(HostStep::Done); }
        return;
    }
    case HostStep::Lever:
        if (!st.isBroken && !st.leverMoving) { HostSays("DONE: the lever repaired the tower", st); HostGo(HostStep::Done); return; }
        if (!g_hostActed) { g_hostActed = true; CT::Use(g_hostTower, me, CT::LookAt{CT::Part::Lever, -1}); }
        if (overdue) { HostSays("its lever never repaired the tower -- FAIL", st); HostGo(HostStep::Done); }
        return;
    default:
        return;
    }
}

// ---- the client: reads the host's, and acts from where it stands -----------------------------------------------

enum class Step : uint8_t { Arm, Break, Open, FarPress, FarPull, Repair, JoinCheck, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
void*    g_tower = nullptr;
int32_t  g_id = -1;
int32_t  g_slot = -1;
bool     g_acted = false;
uint16_t g_actSeq = 0;
std::string g_lightsBefore;

void Go(Step s) {
    g_step = s;
    g_stepMs = NowMs();
    g_acted = false;
}

void Done(const char* verdict) {
    g_step = Step::Done;
    const auto e = coop::fuse_tower_rows::AppliedEdges(coop::net::kFuseTowerKindCoordinate);
    UE_LOGI("[TOWER-DRILL] client DONE %s (ops sent %llu, claims unanswered %zu, reaped %llu, rolls refused %llu; "
            "applied: breaks %u repairs %u presses %u pulls %u inserts %u levers %u fails %u panels %u)", verdict,
            static_cast<unsigned long long>(coop::fuse_tower_ops::OpsSent()), coop::fuse_tower_ops::PendingClaims(),
            static_cast<unsigned long long>(coop::fuse_tower_ops::Reaped()),
            static_cast<unsigned long long>(coop::fuse_tower_ops::RollsRefused()), e.breaks, e.repairs, e.presses,
            e.pulls, e.inserts, e.levers, e.fails, e.panels);
}

void Fail(const char* why) {
    UE_LOGW("[TOWER-DRILL] client: %s -- FAIL", why);
    Done("FAIL");
}

void Abandon(const char* why) {
    UE_LOGW("[TOWER-DRILL] client ABANDONED: %s", why);
    Done("ABANDONED");
}

bool Overdue(uint64_t bound = kCrossMs) { return NowMs() - g_stepMs > bound; }

void FailDiverged(const char* what, const CT::State& l, const FuseTowerRow& w) {
    char why[320];
    std::snprintf(why, sizeof(why), "%s: the host's broken %d fuses %s lights %s, here broken %d fuses %s lights %s",
                  what, RowBroken(w) ? 1 : 0, RowDigits(w, true).c_str(), RowDigits(w, false).c_str(),
                  l.isBroken ? 1 : 0, Digits(l, true).c_str(), Digits(l, false).c_str());
    Fail(why);
}

void Pass(const char* leg, const CT::State& l) {
    UE_LOGI("[TOWER-DRILL] client PASS %s: tower %d broken %d opened %d lever %s fuses %s lights %s", leg, l.id,
            l.isBroken ? 1 : 0, l.opened ? 1 : 0, l.leverUp ? "up" : "down", Digits(l, true).c_str(),
            Digits(l, false).c_str());
}

// The rows have answered my last op.
bool Answered() {
    return static_cast<int16_t>(static_cast<uint16_t>(coop::fuse_tower_ops::LastAck() - g_actSeq)) >= 0;
}

void ClientTick(void* player) {
    CT::State l;
    FuseTowerRow w{};
    const bool have = g_tower && CT::Read(g_tower, l) && HostRow(g_id, w);
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (g_stepMs == 0) g_stepMs = NowMs();
        g_tower = DrillTower();
        g_id = g_tower ? CT::IdOf(g_tower) : -1;
        if (!g_tower || !HostRow(g_id, w)) {
            if (Overdue()) Abandon("no tower here, or the host's rows never came");
            return;
        }
        Census("client");
        Go(IsJoin(ArmOf()) ? Step::JoinCheck : Step::Break);
        return;
    case Step::Break:
        if (have && RowBroken(w) && SameState(l, w)) { Pass("the host's break crossed", l); Go(Step::Open); return; }
        if (Overdue()) {
            if (have) FailDiverged("the host's break never reached this copy", l, w);
            else Fail("the host's break never reached this copy (unread)");
        }
        return;
    case Step::Open:
        if (have && l.opened && !l.isAnim) { Pass("the host's panel opened here", l); Go(Step::FarPress); return; }
        if (Overdue()) Fail("the host's panel never opened here");
        return;
    case Step::FarPress:
        if (!have) return;
        if (!g_acted) {
            g_acted = true;
            g_lightsBefore = Digits(l, false);
            CT::Use(g_tower, player, CT::LookAt{CT::Part::Button, 0});
            g_actSeq = coop::fuse_tower_ops::LastSentSeq();
            UE_LOGI("[TOWER-DRILL] client pressed puzzle button 0 from where it stands (seq %u)", g_actSeq);
            return;
        }
        if (Answered()) {
            if (Digits(l, false) != g_lightsBefore) { Fail("a press from out of reach changed the puzzle"); return; }
            Pass("a press from out of reach was refused, the puzzle unchanged", l);
            Go(Step::FarPull);
            return;
        }
        if (Overdue()) Fail("the host never answered the press");
        return;
    case Step::FarPull:
        if (!have) return;
        if (!g_acted) {
            g_slot = BlownSlot(l);
            if (g_slot < 0 || g_slot >= w.fuseCount || w.fuses[g_slot] != 2) { Abandon("no fuse is blown on both peers"); return; }
            g_acted = true;
            CT::Use(g_tower, player, CT::LookAt{CT::Part::Fuse, g_slot});
            g_actSeq = coop::fuse_tower_ops::LastSentSeq();
            CT::Read(g_tower, l);
            UE_LOGI("[TOWER-DRILL] client pulled fuse %d from where it stands (seq %u): here it reads %d, claims "
                    "unanswered %zu", g_slot, g_actSeq, l.fuses[g_slot], coop::fuse_tower_ops::PendingClaims());
            return;
        }
        if (Answered() && coop::fuse_tower_ops::PendingClaims() == 0) {
            ue_wrap::engine::MainPlayerGrabState gs{};
            void* held = ue_wrap::engine::ReadMainPlayerGrabState(player, gs)
                             ? (gs.grabbingActor ? gs.grabbingActor : gs.holdingActor) : nullptr;
            const bool heldPulled = held && CT::IsPulledFuse(held);
            UE_LOGI("[TOWER-DRILL] client's pull answered: slot %d reads %d (the host's %d), reaped %llu, the hand "
                    "holds %s", g_slot, l.fuses[g_slot], w.fuses[g_slot],
                    static_cast<unsigned long long>(coop::fuse_tower_ops::Reaped()),
                    !held ? "nothing it grabs or carries" : heldPulled ? "a pulled fuse" : "another prop");
            if (l.fuses[g_slot] != 2) { Fail("a pull from out of reach kept its slot empty"); return; }
            if (coop::fuse_tower_ops::Reaped() == 0) { Fail("a refused pull's fuse was not reaped"); return; }
            Pass("a pull from out of reach was refused, its fuse reaped, the slot blown again", l);
            Go(Step::Repair);
            return;
        }
        if (Overdue()) Fail("the host never answered the pull");
        return;
    case Step::Repair: {
        // The lever stands up after a lever's repair; on the two easiest difficulties the insert repairs, and the
        // lever never moves.
        const bool leverUp = (w.flags & coop::net::kFuseTowerLeverUp) != 0;
        if (have && !RowBroken(w) && SameState(l, w) && l.leverUp == leverUp) {
            const auto e = coop::fuse_tower_rows::AppliedEdges(coop::net::kFuseTowerKindCoordinate);
            Pass("the host's repair crossed", l);
            if (e.pulls < 1 || e.inserts < 1 || e.repairs < 1 || e.panels < 1 ||
                (leverUp && (e.levers < 3 || e.fails < 1 || e.presses < 1))) {
                char why[200];
                std::snprintf(why, sizeof(why), "a step of the host's repair was not applied as the tower's own "
                              "(pulls %u inserts %u presses %u levers %u fails %u repairs %u panels %u)", e.pulls,
                              e.inserts, e.presses, e.levers, e.fails, e.repairs, e.panels);
                Fail(why);
                return;
            }
            Done("PASS");
            return;
        }
        if (Overdue(kRepairMs)) {
            if (have) FailDiverged("the host's repair never reached this copy", l, w);
            else Fail("the host's repair never reached this copy (unread)");
        }
        return;
    }
    case Step::JoinCheck:
        if (have && RowBroken(w) && SameState(l, w)) {
            if (coop::fuse_tower_ops::RollsRefused() == 0) {
                Fail("this joiner's tower reads the host's, but it rolled on its own");
                return;
            }
            Pass("the joiner's tower reads the host's, its own roll refused", l);
            Done("PASS");
            return;
        }
        if (Overdue()) {
            if (have) FailDiverged("the joiner's tower never read the host's", l, w);
            else Fail("the joiner's tower never read the host's (unread)");
        }
        return;
    case Step::Done:
        return;
    }
}

}  // namespace

namespace detail {

void CoordHostTick(coop::net::Session& s) { HostTick(s); }

void CoordClientTick() {
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void CoordReset() {
    g_host = HostStep::Arm;
    g_hostMs = 0;
    g_hostTower = nullptr;
    g_hostSlot = -1;
    g_hostActed = false;
    g_step = Step::Arm;
    g_stepMs = 0;
    g_tower = nullptr;
    g_id = g_slot = -1;
    g_acted = false;
    g_actSeq = 0;
}

}  // namespace detail
}  // namespace coop::dev::tower_drill
