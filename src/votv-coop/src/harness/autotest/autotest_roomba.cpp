// harness/autotest/autotest_roomba.cpp -- TEST-ONLY: the robot cleaner is the host's. A verdict is decided by an event; a
// bound passing with none is INVALID. Armed by the row roomba_drill on both peers (VOTVCOOP_CAN_DIR). The host drops two
// cleaners, A and B, in front of itself; B stays off.
//   1     The client walks to the host with the director, then presses A's switch the way the use key does. Its press is
//         asked of the host, which presses its own cleaner. The fence: the client then sends intents for B (off, which B
//         already is) until the host answers with B's state; the host judged them in order, so A's state, if the host
//         switched A, reached the client before B's answer. At B's answer the client's A must have flipped (FAIL there,
//         else). Then A runs and the host's copy drives: once it has moved 200 cm the host switches it off, and at that
//         state (the fence again) the client's copy must have followed at least 100 cm. `rig: VERDICT roomba
//         PASS|FAIL|INVALID`. Reds: roomba_intent_off (client), roomba_drive_claim_off (host).
//   join  With the rig's --rejoin. The host runs A, the client leaves and comes back; at the ready edge the host must
//         have sent the joiner a state, and the rejoined client (VOTVCOOP_ROOMBA_DRILL_REJOIN=1) must read A running on its
//         copy once the state arrived and none is held: `rig: VERDICT roomba-join ...`.

#include "harness/autotest.h"
#include "disc_drills.h"      // co-located private headers (src tree, not include/)
#include "gt_wait.h"
#include "rejoin_steps.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/roomba_sync.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace PS = harness::autotest::peer_signal;
namespace RV = coop::rig_verdict;
namespace RS = coop::roomba_sync;
using disc_drill::OnGameThread;

constexpr const char* kTag = "roomba";
constexpr const char* kTagJoin = "roomba-join";
constexpr int kStepSec = 120;
constexpr float kHostMoveCm = 200.f;   // the host's cleaner has driven this far
constexpr float kCopyMoveCm = 100.f;   // and the client's copy must have followed this far by the switch-off

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::roomba_drill); }

void* CleanerOf(uint32_t eid) { return EL::LiveActorOfType(static_cast<EL::ElementId>(eid), EL::ElementType::Prop); }

bool Active(void* roomba) {
    int32_t off = -1;
    uint8_t mask = 0;
    return roomba && R::FindBoolProperty(R::ClassOf(roomba), L"active", off, mask) && off >= 0 &&
           (*(static_cast<uint8_t*>(roomba) + off) & mask) != 0;
}

// The switch as the use key presses it (action 5): on the host the lane lets it run, on a client it is refused and asked.
bool PressSwitch(void* roomba) {
    ue_wrap::ParamFrame f(R::FindFunction(R::ClassOf(roomba), L"actionOptionIndex"));
    if (!f.valid()) return false;
    f.Set<uint8_t>(L"action", 5);
    return ue_wrap::Call(roomba, f);
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Where(uint32_t eid, ue_wrap::FVector& at) {
    bool ok = false;
    OnGameThread([&] {
        void* r = CleanerOf(eid);
        ok = r && E::TryGetActorLocation(r, at);
    });
    return ok;
}

// HOST: a cleaner (not the one of element id `except`) dropped on the floor `ahead` cm in front of the host's player and
// `side` cm to the right of that line.
bool Drop(float ahead, float side, uint32_t except, uint32_t& eid) {
    bool ok = false;
    OnGameThread([&] {
        void* const me = coop::players::Registry::Get().Local();
        void* const cls = ue_wrap::object_index::ClassByName(L"prop_roomba_C");
        ue_wrap::FVector at{};
        if (!me || !cls || !E::TryGetActorLocation(me, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
        void* r = nullptr;
        const auto d = coop::dev::drill_props::DropOnFloor(me, cls, nullptr, at.X + fwd.X * ahead - fwd.Y * side,
                                                           at.Y + fwd.Y * ahead + fwd.X * side, at.Z, 400.f, 50.f, 20.f, &r);
        ok = d == coop::dev::drill_props::Drop::Spawned && r;
    });
    if (!ok) return false;
    // Enrolled: the drain that mints the element id has run (the host's lane has announced it).
    return WaitGT("the dropped cleaner enrolled", 30, [&] {
        struct Ctx { uint32_t except; uint32_t eid = EL::kInvalidId; } ctx{except};
        if (void* cls = ue_wrap::object_index::ClassByName(L"prop_roomba_C"))
            ue_wrap::object_index::ForEachInstance(cls, [](void* p, void* obj, int32_t) {
                auto& x = *static_cast<Ctx*>(p);
                if (x.eid != EL::kInvalidId || !obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
                const EL::ElementId e = EL::Registry::Get().EidForActor(obj);
                if (e != EL::kInvalidId && static_cast<uint32_t>(e) != x.except) x.eid = static_cast<uint32_t>(e);
            }, &ctx);
        if (ctx.eid != EL::kInvalidId) eid = ctx.eid;
        return ctx.eid != EL::kInvalidId;
    });
}

// ---- 1 ----

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); })) return;
    if (!PS::Await("client_near", kStepSec)) return;
    uint32_t a = 0, b = 0;
    if (!Drop(250.f, 0.f, EL::kInvalidId, a)) return;
    // B is the second one dropped: the first found enrolled cleaner is A, so B is dropped off the line and found by exclusion.
    if (!Drop(250.f, 120.f, a, b)) return;
    if (a == b) {
        RV::Say(kTag, RV::Outcome::Invalid, "the second cleaner could not be told from the first");
        return;
    }
    ue_wrap::FVector start{};
    if (!Where(a, start)) return;
    UE_LOGI("roomba-drill: HOST cleaners A eid %u, B eid %u", a, b);
    PS::Mark("placed", std::to_string(a) + " " + std::to_string(b));
    if (!PS::Await("copies", kStepSec)) return;

    // Arm 1: the client presses A; the host must switch it (the client decides at the fence).
    PS::Mark("press_go");
    std::string early;
    const bool switched = WaitGT("the host's A switched on by the client's press", kStepSec, [&] {
        if (PS::Peek("client_done", early)) return true;   // the client decided at its fence and said so
        void* r = CleanerOf(a);
        return r && Active(r);
    }, false);
    if (!early.empty()) return;
    if (!switched) {
        RV::Say(kTag, RV::Outcome::Invalid, "A was not switched on by the client's press and the client said nothing in %d s", kStepSec);
        return;
    }
    // Arm 2: A drives; at 200 cm the host switches it off, and that state is the fence for the client's copy.
    ue_wrap::FVector now{};
    if (!WaitGT("the host's cleaner driven 200 cm", kStepSec, [&] {
            void* r = CleanerOf(a);
            return r && E::TryGetActorLocation(r, now) && Dist(now, start) >= kHostMoveCm;
        }))
        return;
    UE_LOGI("roomba-drill: HOST A moved %.0f cm -- switched off", Dist(now, start));
    OnGameThread([&] {
        if (void* r = CleanerOf(a)) PressSwitch(r);
    });
    PS::Mark("moved", std::to_string(Dist(now, start)));
    std::string done;
    if (!PS::AwaitText("client_done", done, kStepSec)) return;
    if (done[0] != 'P') return;   // the client said its FAIL where it was decided
    RS::Counters c;
    OnGameThread([&] { c = RS::Read(); });
    RV::Say(kTag, RV::Outcome::Pass, "the client's press switched the host's cleaner, and its copy followed the drive (%u state(s) sent, %u press(es) switched)",
            c.statesSent, c.intentsSwitched);
    UE_LOGI("roomba-drill: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!meet::WalkToHost(kTag, 300.f)) return;
    PS::Mark("client_near");
    std::string text;
    if (!PS::AwaitText("placed", text, kStepSec)) return;
    uint32_t a = 0, b = 0;
    if (sscanf_s(text.c_str(), "%u %u", &a, &b) != 2) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's placed text '%s' does not parse", text.c_str());
        return;
    }
    if (!WaitGT("the copies of A and B", 60, [&] { return CleanerOf(a) && CleanerOf(b); })) return;
    PS::Mark("copies");
    if (!PS::Await("press_go", kStepSec)) return;

    // Arm 1: press A as the use key does; then the fence, B's answer.
    ue_wrap::FVector copyStart{};
    bool pressed = false;
    OnGameThread([&] {
        void* r = CleanerOf(a);
        if (r) E::TryGetActorLocation(r, copyStart);
        pressed = r && PressSwitch(r);
    });
    if (!pressed) {
        RV::Say(kTag, RV::Outcome::Invalid, "the press of A's switch could not be made on the client's copy");
        return;
    }
    // The fence: an intent for B, which is off and stays off, is answered with B's state whatever the host did with A's
    // press, and in order after it. One a pass until the answer, as the host takes one intent a slot every 250 ms.
    const bool answered = WaitGT("the host's answer to the fence intent", kStepSec, [&] {
        coop::net::RoombaPayload p{};
        p.eid = b;
        p.active = 0;
        harness::session_runtime::Session().SendReliable(coop::net::ReliableKind::RoombaIntent, &p, sizeof(p));
        return RS::Read().lastStateEid == b;
    });
    if (!answered) return;
    RS::Counters c;
    bool flipped = false;
    OnGameThread([&] {
        c = RS::Read();
        void* r = CleanerOf(a);
        flipped = r && Active(r);
    });
    if (!flipped) {
        RV::Say(kTag, RV::Outcome::Fail, "the host answered the fence after A's press and A did not switch on here (presses asked of the host=%u, states applied=%u)",
                c.intentsSent, c.statesApplied);
        PS::Mark("client_done", "F");
        return;
    }
    UE_LOGI("roomba-drill: CLIENT A switched on by the host (presses asked=%u)", c.intentsSent);

    // Arm 2: the host switched A off after it had moved; that state is the fence, the copy must have followed.
    std::string moved;
    if (!PS::AwaitText("moved", moved, kStepSec)) return;
    if (!WaitGT("A's switch-off applied here", kStepSec, [] { return RS::Read().statesApplied >= 2; })) return;
    ue_wrap::FVector copyNow{};
    if (!Where(a, copyNow)) {
        RV::Say(kTag, RV::Outcome::Invalid, "A's copy could not be read");
        return;
    }
    const float followed = Dist(copyNow, copyStart);
    UE_LOGI("roomba-drill: CLIENT A's copy moved %.0f cm of the host's %hs", followed, moved.c_str());
    if (followed < kCopyMoveCm) {
        RV::Say(kTag, RV::Outcome::Fail, "the host's cleaner moved %s cm and the host switched it off, and the client's copy moved %.0f cm (at least %.0f)",
                moved.c_str(), followed, kCopyMoveCm);
        PS::Mark("client_done", "F");
        return;
    }
    PS::Mark("client_done", "P");
    UE_LOGI("roomba-drill: DONE on client");
}

// ---- join ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    uint32_t a = 0;
    if (!Drop(250.f, 0.f, EL::kInvalidId, a)) return;
    bool on = false;
    OnGameThread([&] {
        void* r = CleanerOf(a);
        on = r && PressSwitch(r) && Active(r);
    });
    if (!on) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's cleaner could not be switched on");
        return;
    }
    UE_LOGI("roomba-drill: HOST runs cleaner eid %u", a);
    PS::Mark("running", std::to_string(a));
    if (!PS::Await("client_done", kStepSec)) return;
    const uint32_t gen = rejoin::FirstLifeGen();
    PS::Mark("host_ack");
    uint32_t sent0 = 0, sentAtEdge = 0;
    OnGameThread([&] { sent0 = RS::Read().statesSent; });
    if (!rejoin::AwaitNextLifeReady(gen, [&] { sentAtEdge = RS::Read().statesSent - sent0; })) return;
    UE_LOGI("roomba-drill: HOST the rejoined client's world is up; %u state(s) sent at its edge", sentAtEdge);
    // Decided at the edge: a cleaner running and no state told to the joiner cannot change later.
    if (sentAtEdge == 0) {
        RV::Say(kTagJoin, RV::Outcome::Fail, "the host's cleaner was running at the client's ready edge and the host sent the joiner no state");
        return;
    }
    PS::Mark("edge", std::to_string(a));
    std::string done;
    const bool got = PS::AwaitText("rejoin_done", done, kStepSec);
    OnGameThread([&] {
        void* r = CleanerOf(a);
        if (r && Active(r)) PressSwitch(r);   // the host's cleaner back to rest
    });
    if (!got) return;
    if (done[0] == 'P') RV::Say(kTagJoin, RV::Outcome::Pass, "the rejoined client's copy of the running cleaner runs (%u state(s) sent at its edge)", sentAtEdge);
    UE_LOGI("roomba-drill: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!PS::Await("running", kStepSec)) return;
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    rejoin::LeaveForRejoin("roomba drill: life 1 leaves for the rejoin");
    UE_LOGI("roomba-drill: DONE on client");
}

void RunRejoin() {
    if (!disc_drill::HostPuppetUp()) return;
    std::string text;
    if (!PS::AwaitText("edge", text, kStepSec)) return;
    const uint32_t a = static_cast<uint32_t>(std::atoi(text.c_str()));
    // The state arrived and no state waits for an unbound copy: it was applied at the bind or at once.
    if (!WaitGT("the host's state received and none held", kStepSec, [] { return RS::Read().statesReceived >= 1 && RS::Read().held == 0; }) ||
        !WaitGT("the copy of the running cleaner", 60, [&] { return CleanerOf(a) != nullptr; })) {
        PS::Mark("rejoin_done", "I");
        return;
    }
    bool on = false;
    OnGameThread([&] { on = Active(CleanerOf(a)); });
    UE_LOGI("roomba-drill: REJOIN the copy of cleaner eid %u runs=%d", a, on ? 1 : 0);
    if (!on) RV::Say(kTagJoin, RV::Outcome::Fail, "the host's cleaner runs, its state reached the joiner and the joiner's copy does not run");
    PS::Mark("rejoin_done", on ? "P" : "F");
    UE_LOGI("roomba-drill: REJOIN DONE");
}

}  // namespace

DWORD WINAPI RoombaDrillThread(LPVOID /*arg*/) {
    const bool join = Arm() == "join";
    if (!PS::Begin(join ? kTagJoin : kTag)) return 0;
    if (!IsClientRole()) {
        if (join) RunJoinHost();
        else RunHost();
    } else if (!join) {
        RunClient();
    } else if (coop::config::ReadEnv("VOTVCOOP_ROOMBA_DRILL_REJOIN") == "1") {
        RunRejoin();
    } else {
        RunJoinClient();
    }
    return 0;
}

}  // namespace harness::autotest
