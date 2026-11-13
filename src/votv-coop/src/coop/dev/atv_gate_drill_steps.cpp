// coop/dev/atv_gate_drill_steps.cpp -- the steps of the ATV drill before the client leaves, each decided on events of the ATV
// host path (coop/interactables/atv_drill_events.h), never on a clock. A step is a prelude on the host, a setup on every peer,
// the actor's act and a decision on every peer; see atv_gate_drill_detail.h. The step ids are the drill's own: 1 refuel, 2
// repair, 3 fit, 4 take-off, 5 the seated controls, 6 the horn tapped (6h held, 6r its release), 7a the host's refuel with
// the client not driving, 7 with the seated client streaming, 8 the client's burn, 5b the dismount with the brake, 9 the host's burn, 11 a
// forged refuel, 12 a forged duplicate fit, 15 a host state crossing a client's open refuel, then 10 (atv_gate_drill_joiner.cpp)
// a joiner's mirror adopted after the host's state for it.

#include "atv_gate_drill_detail.h"

#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_gate.h"
#include "coop/interactables/atv_host_judge.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/net/protocol.h"

#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

namespace coop::dev::atv_gate_drill::detail {
namespace {

namespace op = coop::net::atv_host_op;
namespace reason = coop::net::atv_host_reason;
namespace AG = coop::atv_gate;
namespace H = coop::atv_host_sync;

constexpr float kFallFuel = 0.01f;   // a block that lowers a copy's fuel by more than this carries a burn
constexpr float kSame = 0.06f;       // two copies' vitals agree within this

bool Ge(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }

std::string Num(uint32_t seq, float fuel, float health, int modules) {
    std::ostringstream o;
    o << seq << ' ' << fuel << ' ' << health << ' ' << modules;
    return o.str();
}

struct Facts {
    uint32_t seq = 0;
    float fuel = 0.f, health = 0.f;
    int modules = 0;
};
bool Parse(const std::string& t, Facts& f) {
    std::istringstream in(t);
    return static_cast<bool>(in >> f.seq >> f.fuel >> f.health >> f.modules);
}

// ---- preludes, setups and acts ----

bool Seed20(Ctx& c, std::string& why) { return SeedHost(c.key, 20.f, 50.f, why); }
bool Seed30(Ctx& c, std::string& why) { return SeedHost(c.key, 30.f, 100.f, why); }

// The host fits the module itself: its copy and every client's hold it before the step's barrier.
bool HostFitsModule(Ctx& c, std::string& why) {
    const int before = ReadVitals(c.key).modules;
    if (!FitModule(c.key, why)) return false;
    if (!WaitUntil("the host's copy holds the module", 20, [&] { return ReadVitals(c.key).modules > before; })) {
        why = "the host's own fit left no module";
        return false;
    }
    return true;
}

bool ActPour(Ctx& c, std::string& why) { return PourCan(c.key, why); }
bool ActToolbox(Ctx& c, std::string& why) { return UseToolbox(c.key, why); }
bool ActFit(Ctx& c, std::string& why) { return FitModule(c.key, why); }
bool ActTake(Ctx& c, std::string& why) { return TakeModuleOff(c.key, why); }

bool ActControls(Ctx& c, std::string& why) {
    return Sit(c.key, why) && Lights(c.key, why) && Brake(c.key, why) && TurboOn(c.key, why);
}
bool ActTap(Ctx& c, std::string& why) { return HornTap(c.key, why); }
bool ActHold(Ctx& c, std::string& why) { return HornPress(c.key, why); }
bool ActRelease(Ctx& c, std::string& why) { return HornRelease(c.key, why); }
bool ActDrive(Ctx& c, std::string& why) { return DriveOn(c.key, why); }
bool ActBrakeAndLeave(Ctx& c, std::string& why) { return Brake(c.key, why) && Leave(c.key, why); }
bool ActHostDrives(Ctx& c, std::string& why) { return Sit(c.key, why) && DriveOn(c.key, why); }

// The forged requests: made as a player's would be, through the open-request gate, with nothing the host's judge may trust.
bool Forge(Ctx& c, uint8_t opcode, float amount, uint8_t module, std::string& why) {
    uint32_t id = 0;
    OnGt([&] { id = AG::SendForDrill(c.key, opcode, amount, module); });
    if (!id) {
        why = "the forged request was not sent";
        return false;
    }
    c.reqId = id;
    return true;
}
bool ActForgeRefuel(Ctx& c, std::string& why) { return EmptyHand(c.key, why) && Forge(c, op::kRefuel, 10.f, 0, why); }
bool ActForgeFit(Ctx& c, std::string& why) {
    const int m = FirstModule(c.key);
    if (m < 0) {
        why = "this client's copy holds no module to forge a duplicate of";
        return false;
    }
    return Forge(c, op::kFit, 0.f, static_cast<uint8_t>(m), why);
}

// The client stays seated from step 5 and does not drive: a seated client syncer streams its blocks at the drive rate
// whether it drives or not (atv_sync's tick), which is all the ack needs, and the ATV stays where the host can aim its
// pour (a moving ATV took no aim: f2-green step 7 INVALID).
bool SetupHold(Ctx& c, std::string&) {
    if (c.me == kClient) OnGt([&] { H::HoldNextStateForDrill(c.key); });
    return true;
}
bool SetupModuleInHand(Ctx& c, std::string& why) { return c.me != kClient || ModuleToHand(c.key, why); }
void TearDownHold(Ctx& c) {
    if (c.me == kClient) OnGt([&] { H::HoldNextStateForDrill(std::wstring()); });
}
void TearDownHostDrive(Ctx& c) {
    if (c.me != kHost) return;
    std::string unused;
    DriveOff(c.key);
    Leave(c.key, unused);
}
void TearDownClientDrive(Ctx& c) {
    if (c.me == kClient) DriveOff(c.key);
}

// ---- decisions ----

// CLIENT: the request the act opened (its id), or the event that the act ran with none opened.
int WaitAsk(Ctx& c, uint8_t opcode, bool& ungated, uint32_t& reqId) {
    DE::Rec r;
    for (int i = 0; i < kEventS * 10; ++i) {
        if (Aborted()) return -1;
        while (DE::Next(DE::kAsked, c.cur[DE::kAsked], r))
            if (r.i0 == opcode) {
                ungated = false;
                reqId = static_cast<uint32_t>(r.i1);
                return 1;
            }
        while (DE::Next(DE::kUngated, c.cur[DE::kUngated], r))
            if (r.i0 == opcode) {
                ungated = true;
                return 1;
            }
        SleepStep();
    }
    if (!Aborted()) End('I', "no request, and no event that none was opened, within " + std::to_string(kEventS) + " s");
    return 0;
}

// An act that asks the host (steps 1-4): the actor's answer, the host's accepted request, the observer's state.
Result DecideRequest(Ctx& c, uint8_t opcode) {
    DE::Rec r;
    if (c.me == kClient) {
        bool ungated = false;
        uint32_t id = 0;
        if (WaitAsk(c, opcode, ungated, id) != 1) return Stopped();
        if (ungated) return Fail("the act ran on this client's copy and no request was opened for it");
        if (WaitRec(c, DE::kAnswer, [&](const DE::Rec& a) { return static_cast<uint32_t>(a.i0) == id; }, r) != 1) return Stopped();
        if (r.i1 != reason::kOk) return Fail("the host refused the request: reason " + std::to_string(r.i1));
        if (opcode == op::kFit && HandHoldsModule()) return Fail("the module was not consumed from this client's hand");
        if (opcode == op::kTakeoff && !HandHoldsModule()) return Fail("the take-off put no module in this client's hand");
        return Pass("the host's answer closed the request");
    }
    if (c.me == kHost) {
        if (WaitRec(c, DE::kAccepted, [&](const DE::Rec& a) { return a.i0 == opcode && a.i1 == kClient; }, r) != 1) return Stopped();
        Fact(c, Num(static_cast<uint32_t>(r.i2), r.f0, r.f1, static_cast<int>(r.f2)));
        const int count = static_cast<int>(r.f2);
        if (opcode == op::kRefuel && !(r.f0 > c.fuel0 + 0.5f)) return Fail("the host's fuel did not rise");
        if (opcode == op::kRepair && !(r.f1 >= 99.5f)) return Fail("the host's health is not 100");
        if (opcode == op::kFit && count != c.modules0 + 1) return Fail("the host holds " + std::to_string(count) + " modules, not one more");
        if (opcode == op::kTakeoff && count != c.modules0 - 1) return Fail("the host holds " + std::to_string(count) + " modules, not one fewer");
        return Pass("the host applied the request");
    }
    std::string t;
    Facts f;
    if (!AwaitFact(c, kHost, t) || !Parse(t, f)) return Stopped();
    if (WaitRec(c, DE::kStateApplied, [&](const DE::Rec& a) { return a.i1 == 0 && Ge(static_cast<uint32_t>(a.i0), f.seq); }, r) != 1)
        return Stopped();
    if (std::fabs(r.f0 - f.fuel) > kSame || std::fabs(r.f1 - f.health) > kSame || r.i2 != f.modules)
        return Fail("this copy's state differs from the host's");
    return Pass("this copy shows the host's state");
}

Result D1(Ctx& c) { return DecideRequest(c, op::kRefuel); }
Result D2(Ctx& c) { return DecideRequest(c, op::kRepair); }
Result D3(Ctx& c) { return DecideRequest(c, op::kFit); }
Result D4(Ctx& c) { return DecideRequest(c, op::kTakeoff); }

// The control bits `expect` reach this copy through the blocks: decided on each block's own event. A bit counts as taken
// when a block made the copy change it (i2) and the copy then shows what the block says (i1): a block the copy already
// agreed with, such as the first of a parked ATV, proves nothing and is no credit.
Result DecideControls(Ctx& c, uint8_t expect) {
    if (c.me == kClient) return Pass("the keys were pressed");
    uint8_t ok = 0, bad = 0;
    DE::Rec r;
    if (WaitRec(c, DE::kControls, [&](const DE::Rec& e) {
            ok |= static_cast<uint8_t>(e.i1 & e.i2);
            bad |= static_cast<uint8_t>(e.i0 & ~e.i1);
            return (bad & expect) != 0 || (ok & expect) == expect;
        }, r) != 1)
        return Stopped();
    if (bad & expect) return Fail("a block's control bits were not taken by this copy (mask " + std::to_string(bad & expect) + ")");
    return Pass("this copy took the block's control bits");
}
Result D5(Ctx& c) { return DecideControls(c, coop::net::kAtvLights | coop::net::kAtvBrake | coop::net::kAtvTurbo); }
Result D5b(Ctx& c) { return DecideControls(c, coop::net::kAtvBrake); }

// The horn is a level in the block's control bits (kAtvHorn): the changes of it this copy took from the blocks, each a
// kControls event with the horn bit in its changed mask and the block's bits in f0. A change counts when the copy sounded it
// (its replay ran, the horn bit of the event's equal mask); one it did not sound is the failure. `downs` rises and `ups`
// falls are waited for.
Result DecideHorn(Ctx& c, int downs, int ups) {
    if (c.me == kClient) return Pass("the horn key was pressed");
    int d = 0, u = 0;
    bool unsounded = false;
    DE::Rec r;
    if (WaitRec(c, DE::kControls, [&](const DE::Rec& e) {
            if (!(e.i0 & coop::net::kAtvHorn)) return false;
            if (!(e.i1 & coop::net::kAtvHorn)) { unsounded = true; return true; }
            ((static_cast<int>(e.f0) & coop::net::kAtvHorn) ? d : u)++;
            return d >= downs && u >= ups;
        }, r) != 1)
        return Stopped();
    if (unsounded) return Fail("a block changed the horn's level and this copy did not sound it");
    return Pass("this copy sounded the horn's change(s) the blocks carried");
}
Result D6(Ctx& c) { return DecideHorn(c, 1, 1); }
Result D6h(Ctx& c) { return DecideHorn(c, 1, 0); }
Result D6r(Ctx& c) { return DecideHorn(c, 0, 1); }

// 7a: the host refuels with the client not driving. No event comes of a publish that never happened, so the client sends a
// forged request after it: the reliable lane is in order, so the host's state, if it was sent, is applied before the answer.
bool NewerState(const DE::Rec& r, int32_t seq0) { return r.i1 == 0 && Ge(static_cast<uint32_t>(r.i0), static_cast<uint32_t>(seq0) + 1u); }
bool NewerGated(const DE::Rec& r, int32_t seq0) { return Ge(static_cast<uint32_t>(r.i0), static_cast<uint32_t>(seq0) + 1u); }

Result D7a(Ctx& c) {
    DE::Rec r;
    if (c.me == kHost) {
        uint32_t seq = 0;
        OnGt([&] { seq = H::CurrentStateSeq(c.key); });
        Fact(c, Num(seq, ReadVitals(c.key).fuel, 0.f, 0));
        return Pass("the host poured");
    }
    std::string t;
    Facts f;
    if (!AwaitFact(c, kHost, t) || !Parse(t, f)) return Stopped();
    if (c.me == kObserver) {
        if (WaitRec(c, DE::kStateApplied, [&](const DE::Rec& a) { return NewerState(a, static_cast<int32_t>(c.seq0)); }, r) != 1)
            return Stopped();
        return Pass("this copy applied the host's refuel");
    }
    std::string why;
    if (!Forge(c, op::kRefuel, 1.f, 0, why)) return Invalid(why);
    const uint32_t id = c.reqId;
    if (WaitRec(c, DE::kAnswer, [&](const DE::Rec& a) { return static_cast<uint32_t>(a.i0) == id; }, r) != 1) return Stopped();
    // A state that reached this copy gated (the forged request opened the gate first) is as much a publish as an applied one.
    if (!DE::AnyBefore(DE::kStateApplied, c.cur[DE::kStateApplied], r.ord, &NewerState, static_cast<int32_t>(c.seq0)) &&
        !DE::AnyBefore(DE::kStateGated, c.cur[DE::kStateGated], r.ord, &NewerGated, static_cast<int32_t>(c.seq0)))
        return Fail("the host's refuel was never published: no state newer than the step's start reached this copy before the answer");
    return Pass("the host's refuel reached this copy before the answer");
}

// 7: the same refuel while the seated client streams, its state held until its next block is built: that block carries the ack of the
// state before. Decided on the host at the first block of the client's admitted after the refuel.
Result D7(Ctx& c) {
    DE::Rec r;
    if (c.me == kHost) {
        DE::Rec pub;
        if (WaitRec(c, DE::kPublished, [](const DE::Rec&) { return true; }, pub) != 1) return Stopped();
        Fact(c, Num(static_cast<uint32_t>(pub.i0), pub.f0, pub.f1, pub.i1));
        if (WaitRec(c, DE::kBlock, [&](const DE::Rec& b) { return b.ord > pub.ord && b.i0 == 0; }, r) != 1) return Stopped();
        if (r.f1 < r.f0 - 0.3f)
            return Fail("a block the client sent before applying the refuel lowered the host's fuel from " + std::to_string(r.f0) + " to " + std::to_string(r.f1));
        return Pass("the unacked block left the host's refuelled fuel alone");
    }
    std::string t;
    Facts f;
    if (!AwaitFact(c, kHost, t) || !Parse(t, f)) return Stopped();
    if (WaitRec(c, DE::kStateApplied, [&](const DE::Rec& a) { return a.i1 == 0 && Ge(static_cast<uint32_t>(a.i0), f.seq); }, r) != 1)
        return Stopped();
    return Pass("this copy applied the host's refuel");
}

// 8 and 9 (decided against this copy's fuel at the barrier, a state no ring overrun can lose: the burn's own blocks come
// during the act, before the decision reads, f4/f5-green): a burn reaches the copy that does not drive: the host through the client's acked blocks, the others through the
// host's blocks. Decided on the block that lowers the copy's fuel.
Result DecideBurn(Ctx& c, Role driver) {
    uint32_t applied = 0, current = 0;
    OnGt([&] { applied = H::AppliedStateSeq(c.key); current = H::CurrentStateSeq(c.key); });
    if (c.me == driver) {
        UE_LOGI("atv-drill: the driver's copy acks state #%u (its own newest applied)", applied);
        return Pass("this copy drives");
    }
    DE::Rec r, last;
    int seen = 0;
    const bool onHost = c.me == kHost;
    if (WaitRec(c, DE::kBlock, [&](const DE::Rec& b) {
            if (b.i0 == (onHost ? 0 : 1)) { last = b; ++seen; }
            return onHost ? (b.i0 == 0 && b.i1 == 1 && b.f1 < c.fuel0 - kFallFuel) : (b.i0 == 1 && b.f2 < c.fuel0 - kFallFuel);
        }, r) != 1) {
        // What the blocks said while no burn came: the instrument for a burn that never reached this copy.
        UE_LOGI("atv-drill: no burn reached this copy: %d blocks seen, the last acked=%d copy %.2f -> %.2f, its own %.2f; "
                "this peer's state #%u current, #%u applied", seen, last.i1, last.f0, last.f1, last.f2, current, applied);
        return Stopped();
    }
    return Pass("a block carried the burn into this copy");
}
Result D8(Ctx& c) { return DecideBurn(c, kClient); }
Result D9(Ctx& c) { return DecideBurn(c, kHost); }

// 11: a forged refuel with an empty hand is refused, and the answer is seen.
Result D11(Ctx& c) {
    if (c.me != kClient) return Pass("nothing to decide here");
    DE::Rec r;
    const uint32_t id = c.reqId;
    if (WaitRec(c, DE::kAnswer, [&](const DE::Rec& a) { return static_cast<uint32_t>(a.i0) == id; }, r) != 1) return Stopped();
    if (r.i1 != reason::kNoItem)
        return Fail(r.i1 == reason::kOk ? "the host accepted a refuel from an empty hand" : "refused for reason " + std::to_string(r.i1) + ", not no_item");
    // The answer's own state: the refused request, a pour of 10, left the host's fuel where this copy had it at the barrier.
    if (std::fabs(r.f0 - c.fuel0) > 0.5f)
        return Fail("the refused refuel moved the host's fuel from " + std::to_string(c.fuel0) + " to " + std::to_string(r.f0));
    return Pass("refused: no_item, the host's fuel unchanged");
}

// 12: a forged duplicate fit is refused as a duplicate; the host refunds the module.
Result D12(Ctx& c) {
    DE::Rec r;
    if (c.me == kClient) {
        const uint32_t id = c.reqId;
        if (WaitRec(c, DE::kAnswer, [&](const DE::Rec& a) { return static_cast<uint32_t>(a.i0) == id; }, r) != 1) return Stopped();
        if (r.i1 != reason::kDuplicate)
            return Fail(r.i1 == reason::kOk ? "the host fitted a second module" : "refused for reason " + std::to_string(r.i1) + ", not duplicate");
        // The answer's own state: one of that module on the host, as before the forged fit.
        if (static_cast<int>(r.f2) != c.modules0)
            return Fail("the answer shows " + std::to_string(static_cast<int>(r.f2)) + " modules, not the " + std::to_string(c.modules0) + " the host fitted");
        return Pass("refused: duplicate, one module on the host");
    }
    if (c.me != kHost) return Pass("nothing to decide here");
    if (WaitRec(c, DE::kRefund, [](const DE::Rec&) { return true; }, r) != 1) return Stopped();
    if (!r.i0) return Fail("the refused fit's module was not refunded");
    if (ReadVitals(c.key).modules != c.modules0)
        return Fail("the host's copy holds " + std::to_string(ReadVitals(c.key).modules) + " modules, not the " + std::to_string(c.modules0) + " it had");
    return Pass("the refused fit's module was refunded and the host holds one module");
}

// 15: the client pours and its request is held on the host until the host has published a change of its own, so that state
// crosses the client's open request. The copy must not show it while the gate is shut; decided on the state's arrival.
bool SetupHoldRequests(Ctx& c, std::string& /*why*/) {
    if (c.me == kHost) OnGt([&] { coop::atv_host_judge::HoldRequestsForDrill(c.key); });
    return true;
}

Result D15(Ctx& c) {
    if (c.me == kObserver) return Pass("nothing to decide here");
    if (c.me == kHost) {
        std::string why;
        if (!SeedHost(c.key, 10.f, 100.f, why)) return Invalid(why);
        return Pass("the host changed its fuel while the client's refuel waited");
    }
    bool ungated = false;
    uint32_t id = 0;
    if (WaitAsk(c, op::kRefuel, ungated, id) != 1) return Stopped();
    if (ungated) return Fail("the pour ran on this client's copy and no request was opened for it");
    DE::Rec r;
    int crossed = 0;
    for (int i = 0; i < kEventS * 10; ++i) {
        if (Aborted()) return Stopped();
        while (DE::Next(DE::kStateGated, c.cur[DE::kStateGated], r)) {
            ++crossed;
            if (r.i1) return Fail("a host state (fuel " + std::to_string(r.f0) + ") was applied while the gate was shut: the copy showed " + std::to_string(r.f1));
        }
        while (DE::Next(DE::kAnswer, c.cur[DE::kAnswer], r)) {
            if (static_cast<uint32_t>(r.i0) != id) continue;
            if (!crossed) return Invalid("the host's state did not cross while the gate was shut");
            return Pass("the gate held the copy's own value until the answer");
        }
        SleepStep();
    }
    End('I', "no answer to the pour within " + std::to_string(kEventS) + " s");
    return Stopped();
}

}  // namespace

const Step kSteps[] = {
    {"1", kClient, "a client's refuel", &Seed20, nullptr, &ActPour, &D1, nullptr},
    {"2", kClient, "a client's repair", nullptr, nullptr, &ActToolbox, &D2, nullptr},
    {"3", kClient, "a client's module fitted", nullptr, nullptr, &ActFit, &D3, nullptr},
    {"4", kClient, "a client's module taken off", nullptr, nullptr, &ActTake, &D4, nullptr},
    {"5", kClient, "the seated client's lights, brake and turbo", nullptr, nullptr, &ActControls, &D5, nullptr},
    {"6", kClient, "the syncer's horn, tapped (press and release in one block)", nullptr, nullptr, &ActTap, &D6, nullptr},
    {"6h", kClient, "the syncer's horn held", nullptr, nullptr, &ActHold, &D6h, nullptr},
    {"6r", kClient, "the held horn released", nullptr, nullptr, &ActRelease, &D6r, nullptr},
    {"7a", kHost, "the host's refuel, the client not driving", &Seed30, nullptr, &ActPour, &D7a, nullptr},
    {"7", kHost, "the host's refuel while the seated client streams", &Seed30, &SetupHold, &ActPour, &D7, &TearDownHold},
    {"8", kClient, "the client's burn reaches the host", nullptr, nullptr, &ActDrive, &D8, &TearDownClientDrive},
    {"5b", kClient, "the client dismounts with the brake set", nullptr, nullptr, &ActBrakeAndLeave, &D5b, nullptr},
    {"9", kHost, "the host's burn reaches the clients", nullptr, nullptr, &ActHostDrives, &D9, &TearDownHostDrive},
    {"11", kClient, "a forged refuel with an empty hand", nullptr, nullptr, &ActForgeRefuel, &D11, nullptr},
    {"12", kClient, "a forged duplicate fit", &HostFitsModule, &SetupModuleInHand, &ActForgeFit, &D12, nullptr},
    {"15", kClient, "a host state crossing a client's open refuel", &Seed30, &SetupHoldRequests, &ActPour, &D15, nullptr},
    {"10", kHost, "a joiner's mirror adopted after the host's state for it", &SpawnSecondAtv, nullptr, &ActNone, &D10, nullptr},
};
const int kStepCount = static_cast<int>(sizeof(kSteps) / sizeof(kSteps[0]));

}  // namespace coop::dev::atv_gate_drill::detail
