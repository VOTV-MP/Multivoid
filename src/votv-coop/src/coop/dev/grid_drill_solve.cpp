// coop/dev/grid_drill_solve.cpp -- see grid_drill_solve.h.

#include "grid_drill_solve.h"

#include "grid_drill_checks.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/device_occupancy.h"  // the panel's claim
#include "coop/net/protocol.h"
#include "coop/world/power_grid.h"
#include "coop/world/power_puzzle.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/device_screen.h"  // the panel's claim key, the interface's exit
#include "ue_wrap/devices/generator.h"
#include "ue_wrap/devices/generator_panel.h"

#include <chrono>
#include <cstdint>
#include <string>

namespace coop::dev::grid_drill {
namespace {

namespace GEN = ue_wrap::generator;
namespace GP  = ue_wrap::generator_panel;

// Failure bound only (the shared ones are in grid_drill_checks.h).
constexpr uint64_t kSolveBoundMs = 60000;  // a solve is some ninety inputs, a click's move 0.1-0.2 s

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

enum class Leg : uint8_t {
    Arm, Enter, Claim, Work, Sync, Press, Ack, FloodProbe, FloodProbeAck, Flood, FloodAck, Done
};
Leg        g_leg = Leg::Done;
SolveState g_outcome = SolveState::Ended;  // what a Done leg reports
bool       g_flood = false;
uint64_t   g_legMs = 0;
bool       g_switchLsb = false;  // the solve's reading of the switch target: least significant bit first
uint8_t    g_probeFrom = 0;      // the offset knob before the flood's probe input

void Go(Leg l) {
    g_leg = l;
    g_legMs = NowMs();
}

void Abandon(const char* why) {
    UE_LOGW("[GRID-DRILL] ABANDONED on the client: %s", why);
    g_leg = Leg::Done;
}

void Fail(const char* why) {
    UE_LOGW("[GRID-DRILL] FAIL on the client: %s", why);
    g_leg = Leg::Done;
}

void Finish(SolveState s) {
    g_outcome = s;
    g_leg = Leg::Done;
}

void Run(void* player) {
    const uint64_t now = NowMs();
    switch (g_leg) {
    case Leg::Arm: {
        // This client stands beside the drill's generator, which the host broke as this client's world became
        // ready: it must hold the host's puzzle before it touches it.
        bool broken = false;
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (!DrillGenBroken(broken) || !broken || !PuzzleMatches(why, canon)) {
            if (now - g_legMs > kRowBoundMs) Fail("the host's break and its puzzle never reached this copy");
            return;
        }
        if (!Say("client", "beside the broken generator, holding its puzzle")) { g_leg = Leg::Done; return; }
        Go(Leg::Enter);
        return;
    }
    case Leg::Enter: {
        void* knobs = GP::PanelOf(DrillGen());
        if (!knobs || !GP::Enter(knobs, player)) { Abandon("the panel's use did not run"); return; }
        Go(Leg::Claim);
        return;
    }
    case Leg::Claim: {
        // Inside the panel's interface the device lock claims it, and the host takes this client's inputs only then.
        void* knobs = GP::PanelOf(DrillGen());
        const std::wstring key = knobs ? ue_wrap::device_screen::ClassifyDeviceActorClaimKey(knobs) : std::wstring();
        if (key.empty() || !coop::device_occupancy::LocalHolds(key.c_str())) {
            if (now - g_legMs > kAckBoundMs) Abandon("this client never held the panel's claim");
            return;
        }
        UE_LOGI("[GRID-DRILL] client: inside the panel, holding its claim %ls", key.c_str());
        g_switchLsb = false;
        Go(g_flood ? Leg::FloodProbe : Leg::Work);
        return;
    }
    case Leg::FloodProbe: {
        // One input first, which the host must take before the flood goes: it takes a client's inputs once it has the
        // client's body and its claim on the panel, and an input waiting for either would hold the flood behind it.
        GP::Puzzle p{};
        if (!GP::Read(GP::PanelOf(DrillGen()), p)) { Abandon("the drill's generator's panel is unread"); return; }
        g_probeFrom = p.sine[0];
        coop::power_puzzle::DevFlood(GEN::IndexOf(DrillGen()), 1);
        Go(Leg::FloodProbeAck);
        return;
    }
    case Leg::FloodProbeAck: {
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_legMs > kAckBoundMs) Abandon("the host never answered the probe input");
            return;
        }
        GP::Puzzle p{};
        if (!GP::Read(GP::PanelOf(DrillGen()), p) || p.sine[0] == g_probeFrom) {
            Abandon("the host refused the probe input (its log says why)");
            return;
        }
        Go(Leg::Flood);
        return;
    }
    case Leg::Flood: {
        // What a modified client sends: a burst of inputs at once, past its own send rate. The host must take them
        // within its rate for inputs and broadcast the rows coalesced; its own lines judge both.
        const uint64_t sent0 = coop::power_puzzle::InputsSent();
        coop::power_puzzle::DevFlood(GEN::IndexOf(DrillGen()), kFloodInputs);
        if (coop::power_puzzle::InputsSent() != sent0 + kFloodInputs) { Abandon("the flood did not go"); return; }
        UE_LOGI("[GRID-DRILL] client: sent %d inputs at once", kFloodInputs);
        Go(Leg::FloodAck);
        return;
    }
    case Leg::FloodAck: {
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_legMs > kSolveBoundMs) {
                const std::string what = "the host never answered the flood: " + why;
                Fail(what.c_str());
            }
            return;
        }
        Finish(SolveState::Flooded);
        return;
    }
    case Leg::Work: {
        // One input a tick, as a hand makes them: the wheel over a knob toward its target, a rotator's click toward
        // the turn its grid was built at, a switch's click toward its bit of the target byte, each click waiting for
        // the move before it. The switches' bit order is the one the panel then reads back as complete.
        void* knobs = GP::PanelOf(DrillGen());
        GP::Puzzle p{};
        if (!knobs || !GP::Read(knobs, p)) { Abandon("the drill's generator's panel is unread"); return; }
        if (now - g_legMs > kSolveBoundMs) { Fail("the puzzle was not solved in time"); return; }
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
        Go(Leg::Sync);
        return;
    }
    case Leg::Sync: {
        // The host must hold this client's solution before the press it rests on: every input taken, its
        // canonical this copy's values.
        std::string why;
        coop::net::PowerGridPuzzle canon{};
        if (coop::power_puzzle::UntakenInputs() != 0 || !PuzzleMatches(why, canon)) {
            if (now - g_legMs > kAckBoundMs) {
                const std::string what = "the host never took this client's solution: " + why;
                Fail(what.c_str());
            }
            return;
        }
        if (!Say("client", "the host holds this client's solution")) { g_leg = Leg::Done; return; }
        Go(Leg::Press);
        return;
    }
    case Leg::Press: {
        void* gen = DrillGen();
        const uint64_t sent0 = coop::power_grid::ClientOpsSent();
        if (!GEN::PressActivate(gen, player)) { Abandon("the Activate press did not run"); return; }
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) { Fail("the Activate press left its generator broken here"); return; }
        if (coop::power_grid::ClientOpsSent() == sent0) { Fail("the Activate press sent the host nothing"); return; }
        UE_LOGI("[GRID-DRILL] client: pressed Activate beside the generator, its puzzle solved");
        Go(Leg::Ack);
        return;
    }
    case Leg::Ack: {
        if (coop::power_grid::PendingOps() != 0) {
            if (now - g_legMs > kAckBoundMs) Abandon("the host never answered the press");
            return;
        }
        bool broken = true;
        if (!DrillGenBroken(broken) || broken) {
            Fail("the host refused a press made beside the generator on a puzzle this client solved");
            return;
        }
        Finish(SolveState::Repaired);
        return;
    }
    case Leg::Done:
        return;
    }
}

}  // namespace

void SolveBegin(bool flood) {
    g_flood = flood;
    g_outcome = SolveState::Ended;
    Go(Leg::Arm);
}

SolveState SolveTick(void* player) {
    if (g_leg != Leg::Done) Run(player);
    return g_leg == Leg::Done ? g_outcome : SolveState::Running;
}

}  // namespace coop::dev::grid_drill
