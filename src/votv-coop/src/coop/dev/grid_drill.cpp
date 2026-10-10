// coop/dev/grid_drill.cpp -- see coop/dev/grid_drill.h.

#include "coop/dev/grid_drill.h"

#include "grid_drill_checks.h"  // co-located private header (src tree, not include/)
#include "grid_drill_upgrade.h"  // the upgrade arms' own legs

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/interactables/device_occupancy.h"  // the panel's claim
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/power_grid.h"
#include "coop/world/power_panel.h"
#include "coop/world/power_puzzle.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/device_screen.h"  // the panel's claim key, the interface's exit
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/devices/generator_panel.h"
#include "ue_wrap/devices/laptop.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/engine/engine.h"            // TryGetActorLocation
#include "ue_wrap/engine/engine_component.h"  // GetComponentLocation

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::grid_drill {
namespace {

namespace PC  = ue_wrap::power_control;
namespace GEN = ue_wrap::generator;
namespace GP  = ue_wrap::generator_panel;

constexpr uint8_t  kCalcBit       = 0x08;    // the calc breaker: the lockout's end switches the servers with it
constexpr float    kPressReachCm  = 150.f;   // a lever, within the look-at trace's 200
// The host's reach for a generator op (coop/world/power_grid): a repair pressed within it must be taken; one pressed
// beyond anything its pads add (the generator's bounds and the puppet's lag, a few hundred uu) must be refused.
constexpr float    kGeneratorReachUU = 400.f;
constexpr float    kBeyondPadsUU     = 5000.f;
constexpr uint64_t kLockPollMs       = 250;     // the lock legs read the servers at this cadence
constexpr int      kWalkDeadlineS = 240;
// Failure bounds only: each leg ends on the state it waits for, and these say it never came.
constexpr uint64_t kAckBoundMs    = 15000;
constexpr uint64_t kRowBoundMs    = 30000;
constexpr uint64_t kLockBoundMs   = 90000;   // the desk virus's lockout lasts 60 s on the host
constexpr uint64_t kSolveBoundMs  = 60000;   // a solve is some ninety inputs, a click's move 0.1-0.2 s
// The flood: more than one burst of the host's rate for inputs, fewer than a burst and a full queue, so every input
// is taken, some at the rate.
constexpr int      kFloodInputs   = 40;

enum class Arm : uint8_t { Off, Run, Red, Join, Lockout, LockJoin, Puzzle, PuzzleRed, PuzzleSolve, PuzzleFlood,
                          Upgrade };
Arm Mode() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveString(::coop::config_registry::rows::grid_drill);
        return v == "run" ? Arm::Run : v == "red" ? Arm::Red : v == "join" ? Arm::Join : v == "lockout" ? Arm::Lockout
             : v == "lockjoin" ? Arm::LockJoin : v == "puzzle" ? Arm::Puzzle : v == "puzzlered" ? Arm::PuzzleRed
             : v == "puzzlesolve" ? Arm::PuzzleSolve : v == "puzzleflood" ? Arm::PuzzleFlood
             : (v == "upgrade" || v == "upgradered") ? Arm::Upgrade
             : Arm::Off;
    }();
    return a;
}
bool LockArm() { return Mode() == Arm::Lockout || Mode() == Arm::LockJoin; }
bool PuzzleArm() { return Mode() == Arm::Puzzle || Mode() == Arm::PuzzleRed; }
bool BesideArm() { return Mode() == Arm::PuzzleSolve || Mode() == Arm::PuzzleFlood; }

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- the client's legs -----------------------------------------------------------------------------------------

enum class Step : uint8_t {
    Arm, WalkPanel, Press, Ack, WaitBreak, Repair, RepairAck, HostRepair, LockStart, LockEnd,
    PuzzleMatch, PuzzleHost, PuzzleOwn, PuzzleOwnAck, SolveArm, SolveEnter, SolveClaim, SolveWork, SolveSync,
    SolvePress, SolveAck, FloodProbe, FloodProbeAck, Flood, FloodAck, Done
};
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_nextLockReadMs = 0;
int      g_presses = 0;
uint8_t  g_maskBefore = 0;   // the breakers as the last press went
bool     g_repairFar = false;  // the repair was pressed beyond the Activate button's reach
coop::net::PowerGridPuzzle g_puzzleSeen{};  // the host's puzzle as the last puzzle leg read it
bool     g_switchLsb = false;  // the solve's reading of the switch target: least significant bit first
uint8_t  g_probeFrom = 0;      // the offset knob before the flood's probe input
std::shared_ptr<coop::director::BackgroundWalk> g_walk;  // the director blocks, so the walk runs on a worker

// A walk by the director to `to`, polled: 0 while it walks, 1 once there, 2 when it failed.
int WalkTo(const ue_wrap::FVector& to) {
    if (!g_walk) {
        g_walk = coop::director::StartBackgroundWalk(to, kPressReachCm, kWalkDeadlineS);
        return 0;
    }
    const int st = g_walk->state.load();
    if (st != 0) g_walk.reset();
    return st;
}

// One target in the census: where it stands, how far from this client, and where the navmesh route toward it
// ends (a route that ends short of it names the nearest ground a walker reaches).
void SayTarget(void* player, const ue_wrap::FVector& me, const char* what, bool read, const ue_wrap::FVector& at) {
    if (!read) {
        UE_LOGI("[GRID-DRILL] client census: %s is unread", what);
        return;
    }
    std::vector<ue_wrap::FVector> route;
    const bool routed = coop::director::RouteFrom(player, me, at, &route);
    const ue_wrap::FVector end = routed ? route.back() : ue_wrap::FVector{};
    UE_LOGI("[GRID-DRILL] client census: %s at (%.0f, %.0f, %.0f), %.0f uu away; the route toward it %s "
            "(%.0f, %.0f, %.0f), %.0f uu short of it", what, at.X, at.Y, at.Z, Dist(me, at),
            routed ? "ends at" : "does not exist", end.X, end.Y, end.Z, routed ? Dist(end, at) : -1.f);
}

// Where this client stands against what it presses: the panel's levers, the laptop that holds the breaker page,
// the drill's generator's Activate button. The host judges each press's reach from its puppet of this player.
void SayCensus(void* player, void* panel) {
    ue_wrap::FVector me{}, at{};
    ue_wrap::engine::TryGetActorLocation(player, me);
    UE_LOGI("[GRID-DRILL] client census: this client at (%.0f, %.0f, %.0f)", me.X, me.Y, me.Z);
    void* lever = PC::Lever(panel, kLightBit);
    SayTarget(player, me, "the panel's light lever", lever != nullptr,
              lever ? ue_wrap::engine::GetComponentLocation(lever) : ue_wrap::FVector{});
    void* laptop = ue_wrap::laptop::Instance();
    SayTarget(player, me, "the laptop", laptop && ue_wrap::engine::TryGetActorLocation(laptop, at), at);
    void* button = GEN::ActivateButton(DrillGen());
    SayTarget(player, me, "the drill's generator's Activate button", button != nullptr,
              button ? ue_wrap::engine::GetComponentLocation(button) : ue_wrap::FVector{});
}

void Go(Step s) {
    g_step = s;
    g_stepMs = NowMs();
}

void Abandon(const char* why) {
    UE_LOGW("[GRID-DRILL] ABANDONED on the client: %s", why);
    g_step = Step::Done;
}

void Fail(const char* why) {
    UE_LOGW("[GRID-DRILL] FAIL on the client: %s", why);
    g_step = Step::Done;
}

// The lockout on this copy: the panel disabled while the host's lockout runs; after it, enabled with the servers
// on exactly when the calc breaker is. The servers are not judged during it: the lockout's start switches them
// off, and each server's own check re-lights its loop from usesp_calc within seconds, on the host as here (the
// host's read 30/30 servers 27 s into its lockout). False with `why` otherwise.
bool LockoutHolds(void* panel, bool locked, std::string& why) {
    uint8_t mask = 0;
    bool disabled = false;
    PC::ServerState sv{};
    if (!PC::ReadPress(panel, mask) || !PC::ReadDisabled(panel, disabled) || !PC::ReadServers(panel, sv)) {
        why = "the panel or its servers are unread";
        return false;
    }
    const bool on = (mask & kCalcBit) != 0;
    char buf[160];
    if (disabled != locked || (!locked && (sv.total == 0 || sv.active != (on ? sv.total : 0)))) {
        std::snprintf(buf, sizeof(buf), "disabled=%d servers %d/%d, wanted disabled=%d%s", disabled, sv.active,
                      sv.total, locked ? 1 : 0, locked ? "" : on ? " servers all on" : " servers all off");
        why = buf;
        return false;
    }
    return true;
}

void ClientTick(void* player) {
    const uint64_t now = NowMs();
    void* panel = PC::Panel();
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (!panel || !PC::Lever(panel, kLightBit)) return;  // the panel resolves with the world
        if (g_stepMs == 0) g_stepMs = now;
        if (Mode() == Arm::Join) {
            // The host broke the drill's generator before this client came in: the rows and the canonical at its
            // world-ready must already hold the blackout here.
            bool broken = false;
            if (!DrillGenBroken(broken) || !broken) {
                if (now - g_stepMs > kRowBoundMs) Fail("joined into the host's blackout, its generator is whole");
                return;
            }
            if (Say("client", "joined into the blackout")) SayDone();
            g_step = Step::Done;
            return;
        }
        if (LockArm()) {
            Say("client", "armed");
            Go(Step::LockStart);
            return;
        }
        Say("client", "armed");
        SayCensus(player, panel);
        Go(BesideArm() ? Step::SolveArm : Step::WalkPanel);
        return;
    }
    case Step::LockStart:
    case Step::LockEnd: {
        // The host ran the desk virus's lockout, as this client's session began (lockout) or as it was joining
        // (lockjoin): this copy locks with the canonical, and unlocks with it 60 s after the host's start.
        const bool locked = g_step == Step::LockStart;
        if (now < g_nextLockReadMs) return;
        g_nextLockReadMs = now + kLockPollMs;
        std::string why;
        if (!LockoutHolds(panel, locked, why)) {
            if (now - g_stepMs > kLockBoundMs) {
                const std::string what = std::string(locked ? "the lockout never held here: " : "the lockout never "
                                                     "ended here: ") + why;
                Fail(what.c_str());
            }
            return;
        }
        if (!Say("client", locked ? "locked out" : "the lockout ended")) { g_step = Step::Done; return; }
        if (locked) {
            Go(Step::LockEnd);
        } else {
            SayDone();
            g_step = Step::Done;
        }
        return;
    }
    case Step::WalkPanel: {
        // Down to the panel by the director: a lever is pressed where a player stands.
        const int st = WalkTo(ue_wrap::engine::GetComponentLocation(PC::Lever(panel, kLightBit)));
        if (st == 0) return;
        if (st != 1) { Abandon("the walk to the panel failed (no route, or the deadline)"); return; }
        SayCensus(player, panel);
        Go(Step::Press);
        return;
    }
    case Step::Press: {
        // The press as the E dispatch makes it: the panel's own actionOptionIndex with a hit on the lever.
        const uint64_t sent0 = coop::power_panel::ClientPressesSent();
        PC::ReadPress(panel, g_maskBefore);
        if (!PC::PressLever(panel, player, kLightBit)) { Abandon("the lever's press did not run"); return; }
        if (coop::power_panel::ClientPressesSent() == sent0) {
            Fail("the lever's press sent the host nothing");
            return;
        }
        ++g_presses;
        UE_LOGI("[GRID-DRILL] client: pressed the light lever (press %d)", g_presses);
        Go(Step::Ack);
        return;
    }
    case Step::Ack: {
        if (coop::power_panel::PendingPresses() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the press");
            return;
        }
        // The answer acknowledges a refused press too, and reverts it: a taken one leaves the lever flipped.
        uint8_t mask = 0;
        PC::ReadPress(panel, mask);
        if (((mask ^ g_maskBefore) & (1u << kLightBit)) == 0) {
            Fail("the host refused a press (its log says why)");
            return;
        }
        char step[32];
        std::snprintf(step, sizeof(step), "press %d taken", g_presses);
        if (!Say("client", step)) { g_step = Step::Done; return; }
        // The first press is the host's cue to break the drill's generator, the third its cue to repair it; in the
        // puzzle arms the second is its cue to work the broken generator's panel.
        Go(g_presses == 1 ? Step::WaitBreak : g_presses == 2 ? (PuzzleArm() ? Step::PuzzleHost : Step::Repair)
                                                             : Step::HostRepair);
        return;
    }
    case Step::WaitBreak: {
        bool broken = false;
        if (!DrillGenBroken(broken) || !broken) {
            if (now - g_stepMs > kRowBoundMs) Abandon("the host's break never reached this copy");
            return;
        }
        if (!Say("client", "the drill's generator broke")) { g_step = Step::Done; return; }
        Go(PuzzleArm() ? Step::PuzzleMatch : Step::Press);
        return;
    }
    case Step::PuzzleMatch:
    case Step::PuzzleHost: {
        // The break's puzzle, then the host's own inputs on the panel (a rotator's click and a knob's scroll): each
        // must reach this copy as the host's canonical, targets and values alike.
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        const bool matches = PuzzleMatches(why, canon);
        const bool fresh = g_step == Step::PuzzleMatch || !coop::power_puzzle::SamePuzzle(canon, g_puzzleSeen);
        if (!matches || !fresh) {
            if (now - g_stepMs > kRowBoundMs) {
                const std::string what = std::string(g_step == Step::PuzzleMatch ? "the host's puzzle never reached "
                                                     "this copy: " : "the host's inputs never reached this copy: ") +
                                         (matches ? std::string("the host's puzzle never changed") : why);
                Fail(what.c_str());
            }
            return;
        }
        g_puzzleSeen = canon;
        if (!Say("client", g_step == Step::PuzzleMatch ? "the host's puzzle reached this copy"
                                                       : "the host's inputs reached this copy")) {
            g_step = Step::Done;
            return;
        }
        Go(g_step == Step::PuzzleMatch ? Step::Press : Step::PuzzleOwn);
        return;
    }
    case Step::PuzzleOwn: {
        // An input of this client's own, the wheel over the offset knob, from out of the generator's reach: it
        // shows here at once as the prediction, goes to the host, and the host's refusal must roll it back.
        void* knobs = GP::PanelOf(DrillGen());
        GP::Puzzle before{}, after{};
        if (!knobs || !GP::Read(knobs, before)) { Abandon("the drill's generator's panel is unread"); return; }
        const uint64_t sent0 = coop::power_puzzle::InputsSent();
        if (!GP::Scroll(knobs, 0, before.sine[0] < 15 ? 1.f : -1.f) || !GP::Read(knobs, after)) {
            Abandon("the scroll over the panel's knob did not run");
            return;
        }
        if (after.sine[0] == before.sine[0]) { Fail("the scroll left the offset knob where it was"); return; }
        if (coop::power_puzzle::InputsSent() == sent0) { Fail("a local input sent the host nothing"); return; }
        UE_LOGI("[GRID-DRILL] client: turned the offset knob %u -> %u on this copy", before.sine[0], after.sine[0]);
        Go(Step::PuzzleOwnAck);
        return;
    }
    case Step::PuzzleOwnAck: {
        if (coop::power_puzzle::UntakenInputs() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the input");
            return;
        }
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (!PuzzleMatches(why, canon)) {
            const std::string what = "the refused input was not rolled back: " + why;
            Fail(what.c_str());
            return;
        }
        if (!coop::power_puzzle::SamePuzzle(canon, g_puzzleSeen)) {
            Fail("the host took an input sent from out of the generator's reach");
            return;
        }
        if (Say("client", "an input from out of reach refused, rolled back")) SayDone();
        g_step = Step::Done;
        return;
    }
    case Step::SolveArm: {
        // This client stands beside the drill's generator (its stored pose), which the host broke as this client's
        // world became ready: it must hold the host's puzzle before it touches it.
        bool broken = false;
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (!DrillGenBroken(broken) || !broken || !PuzzleMatches(why, canon)) {
            if (now - g_stepMs > kRowBoundMs) Fail("the host's break and its puzzle never reached this copy");
            return;
        }
        if (!Say("client", "beside the broken generator, holding its puzzle")) { g_step = Step::Done; return; }
        Go(Step::SolveEnter);
        return;
    }
    case Step::SolveEnter: {
        void* knobs = GP::PanelOf(DrillGen());
        if (!knobs || !GP::Enter(knobs, player)) { Abandon("the panel's use did not run"); return; }
        Go(Step::SolveClaim);
        return;
    }
    case Step::SolveClaim: {
        // Inside the panel's interface the device lock claims it, and the host takes this client's inputs only then.
        void* knobs = GP::PanelOf(DrillGen());
        const std::wstring key = knobs ? ue_wrap::device_screen::ClassifyDeviceActorClaimKey(knobs) : std::wstring();
        if (key.empty() || !coop::device_occupancy::LocalHolds(key.c_str())) {
            if (now - g_stepMs > kAckBoundMs) Abandon("this client never held the panel's claim");
            return;
        }
        UE_LOGI("[GRID-DRILL] client: inside the panel, holding its claim %ls", key.c_str());
        g_switchLsb = false;
        Go(Mode() == Arm::PuzzleFlood ? Step::FloodProbe : Step::SolveWork);
        return;
    }
    case Step::FloodProbe: {
        // One input first, which the host must take before the flood goes: it takes a client's inputs once it has the
        // client's body and its claim on the panel, and an input waiting for either would hold the flood behind it.
        GP::Puzzle p{};
        if (!GP::Read(GP::PanelOf(DrillGen()), p)) { Abandon("the drill's generator's panel is unread"); return; }
        g_probeFrom = p.sine[0];
        coop::power_puzzle::DevFlood(GEN::IndexOf(DrillGen()), 1);
        Go(Step::FloodProbeAck);
        return;
    }
    case Step::FloodProbeAck: {
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the probe input");
            return;
        }
        GP::Puzzle p{};
        if (!GP::Read(GP::PanelOf(DrillGen()), p) || p.sine[0] == g_probeFrom) {
            Abandon("the host refused the probe input (its log says why)");
            return;
        }
        Go(Step::Flood);
        return;
    }
    case Step::Flood: {
        // What a modified client sends: a burst of inputs at once, past its own send rate. The host must take them
        // within its rate for inputs and broadcast the rows coalesced; its own lines judge both.
        const uint64_t sent0 = coop::power_puzzle::InputsSent();
        coop::power_puzzle::DevFlood(GEN::IndexOf(DrillGen()), kFloodInputs);
        if (coop::power_puzzle::InputsSent() != sent0 + kFloodInputs) { Abandon("the flood did not go"); return; }
        UE_LOGI("[GRID-DRILL] client: sent %d inputs at once", kFloodInputs);
        Go(Step::FloodAck);
        return;
    }
    case Step::FloodAck: {
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_stepMs > kSolveBoundMs) {
                const std::string what = "the host never answered the flood: " + why;
                Fail(what.c_str());
            }
            return;
        }
        if (Say("client", "the host answered every input of the flood")) SayDone();
        g_step = Step::Done;
        return;
    }
    case Step::SolveWork: {
        // One input a tick, as a hand makes them: the wheel over a knob toward its target, a rotator's click toward
        // the turn its grid was built at, a switch's click toward its bit of the target byte, each click waiting for
        // the move before it. The switches' bit order is the one the panel then reads back as complete.
        void* knobs = GP::PanelOf(DrillGen());
        GP::Puzzle p{};
        if (!knobs || !GP::Read(knobs, p)) { Abandon("the drill's generator's panel is unread"); return; }
        if (now - g_stepMs > kSolveBoundMs) { Fail("the puzzle was not solved in time"); return; }
        if (GP::IsMoving(knobs)) return;
        for (int k = 0; k < 3; ++k) {
            if (p.sine[k] == p.targetSine[k]) continue;
            GP::Scroll(knobs, k, p.sine[k] < p.targetSine[k] ? 1.f : -1.f);
            return;
        }
        for (int i = 0; i < GP::kRotators; ++i) {
            if (p.rotators[i] == 0) continue;
            GP::ClickRotator(knobs, i);
            return;
        }
        for (int i = 0; i < GP::kSwitches; ++i) {
            const unsigned want = (p.switchesTarget >> (g_switchLsb ? i : 7 - i)) & 1u;
            if (((p.switches >> i) & 1u) == want) continue;
            GP::ClickSwitch(knobs, i);
            return;
        }
        if (!GP::Solved(knobs)) {
            if (g_switchLsb) { Fail("every value set, and the panel still reads unsolved"); return; }
            g_switchLsb = true;
            return;
        }
        UE_LOGI("[GRID-DRILL] client: solved the puzzle on this copy (the switch target read from its %s bit)",
                g_switchLsb ? "lowest" : "highest");
        ue_wrap::device_screen::ForceExitInterface(player);
        Go(Step::SolveSync);
        return;
    }
    case Step::SolveSync: {
        // The host must hold this client's solution before the press it rests on: every input taken, its
        // canonical this copy's values.
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_stepMs > kAckBoundMs) {
                const std::string what = "the host never took this client's solution: " + why;
                Fail(what.c_str());
            }
            return;
        }
        if (!Say("client", "the host holds this client's solution")) { g_step = Step::Done; return; }
        Go(Step::SolvePress);
        return;
    }
    case Step::SolvePress: {
        void* gen = DrillGen();
        const uint64_t sent0 = coop::power_grid::ClientOpsSent();
        if (!GEN::PressActivate(gen, player)) { Abandon("the Activate press did not run"); return; }
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) { Fail("the Activate press left its generator broken here"); return; }
        if (coop::power_grid::ClientOpsSent() == sent0) { Fail("the Activate press sent the host nothing"); return; }
        UE_LOGI("[GRID-DRILL] client: pressed Activate beside the generator, its puzzle solved");
        Go(Step::SolveAck);
        return;
    }
    case Step::SolveAck: {
        if (coop::power_grid::PendingOps() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the press");
            return;
        }
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) {
            Fail("the host refused a press made beside the generator on a puzzle this client solved");
            return;
        }
        if (Say("client", "the host judged the press on its own copy and repaired")) SayDone();
        g_step = Step::Done;
        return;
    }
    case Step::Repair: {
        // The repair as a player makes it: the puzzle solved on this copy (the drill's shortcut through its three
        // pages), then the Activate button pressed as the E dispatch presses it, from where this client stands.
        // It mends this copy at once, as the prediction, and goes to the host. The generators stand 470-620 m out
        // and the director's walk there stalls on the terrain, so a repair from the panel is out of the button's
        // reach, and the host's refusal must roll the prediction back; one pressed within reach must be taken.
        void* gen = DrillGen();
        void* button = GEN::ActivateButton(gen);
        ue_wrap::FVector me{};
        if (!button || !ue_wrap::engine::TryGetActorLocation(player, me)) {
            Abandon("the drill's generator or this client is unread");
            return;
        }
        const float dist = Dist(me, ue_wrap::engine::GetComponentLocation(button));
        if (dist > kGeneratorReachUU && dist <= kBeyondPadsUU) {
            Abandon("the repair would be pressed between the button's reach and the host's pads");
            return;
        }
        g_repairFar = dist > kBeyondPadsUU;
        UE_LOGI("[GRID-DRILL] client: the repair is pressed %.0f uu from the Activate button, %s its reach", dist,
                g_repairFar ? "beyond" : "within");
        const uint64_t sent0 = coop::power_grid::ClientOpsSent();
        if (!GEN::WritePuzzleSolved(gen)) { Abandon("the drill's generator's puzzle is unread"); return; }
        if (!GEN::PressActivate(gen, player)) { Abandon("the Activate press did not run"); return; }
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) {
            Fail("the Activate press left its generator broken on this copy");
            return;
        }
        if (coop::power_grid::ClientOpsSent() == sent0) { Fail("the Activate press sent the host nothing"); return; }
        UE_LOGI("[GRID-DRILL] client: repaired the drill's generator at its Activate button");
        Go(Step::RepairAck);
        return;
    }
    case Step::RepairAck: {
        if (coop::power_grid::PendingOps() != 0) {
            if (now - g_stepMs > kAckBoundMs) Abandon("the host never answered the repair");
            return;
        }
        bool broken = false;
        if (!DrillGenBroken(broken) || broken != g_repairFar) {
            Fail(g_repairFar ? "the host's answer to a repair beyond reach left its generator whole"
                             : "the host's answer to a repair within reach left its generator broken");
            return;
        }
        if (!Say("client", g_repairFar ? "repair refused, rolled back" : "repair taken")) {
            g_step = Step::Done;
            return;
        }
        if (!g_repairFar) {
            SayDone();
            g_step = Step::Done;
            return;
        }
        Go(Step::Press);  // the third press, made after the rollback held here
        return;
    }
    case Step::HostRepair: {
        // On the third press the host mends the generator itself at its own Activate button: this copy must run
        // that repair as the host's, its turn-on at the generator included.
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) {
            if (now - g_stepMs > kRowBoundMs) Abandon("the host's own repair never reached this copy");
            return;
        }
        bool turnOn = false;
        if (!GEN::ReadLastCue(DrillGen(), turnOn)) { Abandon("the drill's generator's cue is unread"); return; }
        if (!turnOn) {
            Fail("the host's repair ran on this copy without its turn-on at the generator");
            return;
        }
        if (Say("client", "the host's own repair came, with its turn-on")) SayDone();
        g_step = Step::Done;
        return;
    }
    case Step::Done:
        return;
    }
}

// ---- the host's legs -------------------------------------------------------------------------------------------

uint64_t g_hostSeen = 0;
uint64_t g_hostOpsSeen = 0;
bool     g_hostSaidArm = false;
bool     g_hostActed = false;  // the join arms' break, the lockout arms' lockout
uint64_t g_floodMs = 0, g_floodInputs0 = 0, g_floodRows0 = 0;  // the flood's first taken input, and the counts before
uint64_t g_floodRefused0 = 0;
bool     g_floodDone = false;

// The flood's bounds, checked every tick from its first taken input: the inputs taken within the host's rate for
// them, and the rows broadcast at most once a coalescing window however fast the inputs come.
void HostFloodCheck() {
    const uint64_t taken = coop::power_grid::HostInputsTaken();
    const uint64_t rows = coop::power_grid::HostRowsBroadcast();
    const uint64_t now = NowMs();
    if (g_floodDone) return;
    if (coop::power_grid::HostOpsRefused() != g_floodRefused0) {
        UE_LOGW("[GRID-DRILL] FAIL on the host: an input of the probe or the flood was refused (the log above says why)");
        g_floodDone = true;
        return;
    }
    if (g_floodMs == 0) {
        if (taken == g_floodInputs0) {
            g_floodRows0 = rows;
            return;
        }
        g_floodMs = now;
    }
    const uint64_t ms = now - g_floodMs;
    const uint64_t inputs = taken - g_floodInputs0, sends = rows - g_floodRows0;
    const auto inputsAllowed = static_cast<uint64_t>(coop::power_grid::kInputBurst +
                                                     coop::power_grid::kInputPerSecond * static_cast<float>(ms) / 1000.f) + 1;
    const uint64_t sendsAllowed = 2 + ms / coop::power_grid::kPuzzleRowsEveryMs;
    char buf[160];
    if (inputs > inputsAllowed || sends > sendsAllowed) {
        std::snprintf(buf, sizeof(buf), "the flood's %llu inputs taken and %llu rows broadcast in %llu ms, past %llu and %llu",
                      static_cast<unsigned long long>(inputs), static_cast<unsigned long long>(sends),
                      static_cast<unsigned long long>(ms), static_cast<unsigned long long>(inputsAllowed),
                      static_cast<unsigned long long>(sendsAllowed));
        UE_LOGW("[GRID-DRILL] FAIL on the host: %s", buf);
        g_floodDone = true;
        return;
    }
    if (inputs < static_cast<uint64_t>(kFloodInputs) + 1) return;  // the probe and the flood
    g_floodDone = true;
    std::snprintf(buf, sizeof(buf), "took the probe and the flood, %llu inputs in %llu ms, its rows broadcast %llu times",
                  static_cast<unsigned long long>(inputs), static_cast<unsigned long long>(ms),
                  static_cast<unsigned long long>(sends));
    Say("host", buf);
}

void HostTick(coop::net::Session* s) {
    void* panel = PC::Panel();
    if (!g_hostActed) {
        if (Mode() == Arm::Join) {
            // The blackout the joiner must find at its world-ready.
            void* gen = DrillGen();
            if (!gen) return;
            g_hostActed = true;
            GEN::CallBreak(gen);
            Say("host", "broke the drill's generator before the join");
            return;
        }
        if (BesideArm() && s->IsSlotWorldReady(1)) {
            // The generator the client stands beside, broken for it to repair.
            void* gen = DrillGen();
            if (!gen) return;
            g_hostActed = true;
            GEN::CallBreak(gen);
            Say("host", "broke the drill's generator beside the client");
            return;
        }
        // lockjoin: the lockout as the client joins, so it is on at its world-ready and ends 60 s later; lockout:
        // once the client's world is ready.
        const bool now = (Mode() == Arm::LockJoin && s->IsSlotConnected(1) && !s->IsSlotWorldReady(1)) ||
                         (Mode() == Arm::Lockout && s->IsSlotWorldReady(1));
        if (LockArm() && now && panel) {
            g_hostActed = true;
            if (!PC::CallVirusLockout(panel)) UE_LOGW("[GRID-DRILL] ABANDONED on the host: virus_pb did not run");
            Say("host", "ran the desk virus's lockout");
            return;
        }
    }
    if (!s->IsSlotWorldReady(1)) return;
    if (!g_hostSaidArm) {
        g_hostSaidArm = true;
        g_floodInputs0 = coop::power_grid::HostInputsTaken();
        g_floodRefused0 = coop::power_grid::HostOpsRefused();
        Say("host", "armed");
    }
    if (LockArm()) return;
    if (Mode() == Arm::PuzzleFlood) {
        HostFloodCheck();
        return;
    }
    const uint64_t ops = coop::power_grid::HostOpsTaken();
    if (ops != g_hostOpsSeen) {
        g_hostOpsSeen = ops;
        Say("host", "the client's repair taken");
    }
    const uint64_t taken = coop::power_panel::HostPressesTaken();
    if (taken == g_hostSeen) return;
    g_hostSeen = taken;
    char step[32];
    std::snprintf(step, sizeof(step), "press %llu taken", static_cast<unsigned long long>(taken));
    Say("host", step);
    if (taken != 1 && taken != 3 && !(taken == 2 && PuzzleArm())) return;
    void* gen = DrillGen();
    if (!gen) {
        UE_LOGW("[GRID-DRILL] ABANDONED on the host: the drill's generator is unread");
        return;
    }
    if (taken == 1) {
        GEN::CallBreak(gen);
        Say("host", "broke the drill's generator");
        return;
    }
    if (taken == 2) {
        // The host's own inputs on the broken generator's panel, through its handlers as a player's hand runs
        // them: the first rotator's click and the wheel over the offset knob.
        void* knobs = GP::PanelOf(gen);
        GP::Puzzle p{};
        if (!knobs || !GP::Read(knobs, p) || !GP::ClickRotator(knobs, 0) ||
            !GP::Scroll(knobs, 0, p.sine[0] < 15 ? 1.f : -1.f)) {
            UE_LOGW("[GRID-DRILL] ABANDONED on the host: its inputs on the panel did not run");
            return;
        }
        Say("host", "clicked a rotator and turned a knob on the drill's generator's panel");
        return;
    }
    // The client's third press follows its rolled-back repair: the host's own player mends the generator, its
    // puzzle solved as the drill's shortcut, and the rows carry the repair to the client.
    void* me = coop::players::Registry::Get().Local();
    if (!me || !GEN::WritePuzzleSolved(gen) || !GEN::PressActivate(gen, me)) {
        UE_LOGW("[GRID-DRILL] ABANDONED on the host: its own repair did not run");
        return;
    }
    Say("host", "repaired the drill's generator itself");
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (Mode() == Arm::Off || !session || !session->connected()) return;
    if (!PC::EnsureResolved() || !GEN::EnsureResolved()) return;
    if (Mode() == Arm::Upgrade) {
        UpgradeTick(session);
        return;
    }
    if (session->role() == coop::net::Role::Host) {
        HostTick(session);
        return;
    }
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (player) ClientTick(player);
}

void OnDisconnect() {
    ForgetDrillGen();
    UpgradeOnDisconnect();
    g_step = Step::Arm;
    g_stepMs = 0;
    g_nextLockReadMs = 0;
    g_presses = 0;
    g_walk.reset();
    g_hostSeen = 0;
    g_hostOpsSeen = 0;
    g_hostSaidArm = false;
    g_hostActed = false;
    g_floodMs = g_floodInputs0 = g_floodRows0 = g_floodRefused0 = 0;
    g_floodDone = false;
}

}  // namespace coop::dev::grid_drill
